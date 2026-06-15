// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the SourceManager related classes.
 */

#include "cangjie/Basic/SourceManager.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/Utils/FileUtil.h"
#include "cangjie/Utils/SafePointer.h"
#include <optional>

using namespace Cangjie;

size_t Source::PosToOffset(const Position& pos) const
{
    if (pos.line > static_cast<int>(lineOffsets.size())) {
        return buffer.length();
    }
    if (pos.line < 1 || pos.column < 1) {
        return 0;
    }
    size_t index = static_cast<size_t>(pos.line) - 1;
    auto pStart = buffer.data() + lineOffsets[index];
    auto pEnd = buffer.data() + buffer.length();
    size_t columnOffset = 0;
    while (Utils::GetLineTerminatorLength(pStart + columnOffset, pEnd) == 0 &&
        columnOffset < (static_cast<size_t>(pos.column) - 1) && pStart + columnOffset < pEnd) {
        columnOffset++;
    }
    if (columnOffset < (static_cast<size_t>(pos.column) - 1) && pStart + columnOffset < pEnd) {
        // there's line terminator before `pos.column`
        columnOffset += Utils::GetLineTerminatorLength(pStart + columnOffset, pEnd);
    }

    return std::min(lineOffsets[index] + columnOffset, buffer.length());
}

Source::Source(unsigned int fileID, std::string path, std::string buffer, uint64_t fileHash,
    const std::optional<std::string>& packageName)
    : fileID(fileID), path(std::move(path)), buffer(std::move(buffer)), fileHash(fileHash), packageName(packageName)
{
    if (this->buffer.empty()) {
        return;
    }

    // build lineOffsets
    auto pStart = this->buffer.data();
    size_t length = this->buffer.length();
    auto pEnd = this->buffer.data() + length;
    for (auto ptr = pStart; ptr < pEnd;) {
        while (Utils::GetLineTerminatorLength(ptr, pEnd) == 0 && ptr < pEnd) {
            ptr++;
        }
        if (ptr >= pEnd) {
            break;
        }
        ptr += Utils::GetLineTerminatorLength(ptr, pEnd);
        lineOffsets.emplace_back(ptr - pStart);
    }
}

unsigned int SourceManager::GetFileId(
    const std::string& normalizedPath,
    const std::string& buffer,
    uint64_t fileHash,
    std::optional<std::string> packageName,
    bool isCjmpFile,
    bool updateBuffer)
{
    auto existed = filePathToFileIDMap.find(normalizedPath);
    if (existed != filePathToFileIDMap.end()) {
        auto newBuffer = buffer;
        auto fileID = existed->second;
        CJC_ASSERT_WITH_MSG(sources.count(fileID), "No source buffer found for a fileID");
        if (updateBuffer) {
            newBuffer = sources.at(fileID).buffer + buffer;
        }
        sources.at(fileID) =
            Source{fileID, normalizedPath, newBuffer, fileHash, packageName};
        return fileID;
    } else {
        auto fileID = static_cast<unsigned int>(sources.size());
        if (isCjmpFile) {
            // CJMP files can not have incremental file IDs,
            // because 1) Some IDs may already be used for parent source set files
            // 2) Or in neighbour source set.
            // So IDs need to be evaluated based path.
            std::hash<std::string> hasher;
            size_t hashValue = hasher(normalizedPath);
            fileID = static_cast<unsigned int>(hashValue);
        }

        sources.emplace(fileID, Source(fileID, normalizedPath, buffer, fileHash, packageName));
        filePathToFileIDMap.emplace(normalizedPath, fileID);
        return fileID;
    }
}

unsigned int SourceManager::AddSource(
    const std::string& path, const std::string& buffer, std::optional<std::string> packageName, bool isCjmpFile)
{
    // path canonicalize
    std::string normalizePath = FileUtil::Normalize(path);
    // Change fileHash from content hash to path hash.
    uint64_t fileHash = Utils::GetHash(normalizePath);
    return GetFileId(normalizePath, buffer, fileHash, packageName, isCjmpFile, false);
}

unsigned int SourceManager::AppendSource(const std::string& path, const std::string& buffer, bool isCjmpFile)
{
    // path canonicalize
    std::string normalizePath = FileUtil::Normalize(path);
    uint64_t fileHash = Utils::GetHash(normalizePath);
    return GetFileId(normalizePath, buffer, fileHash, std::nullopt, isCjmpFile, true);
}

bool SourceManager::IsInMacroCallSourceFile(const Position& pos) const
{
    if (pos.fileID == 0 || pos.fileID >= sources.size()) {
        return false;
    }
    return FileUtil::GetFileExtension(sources.at(pos.fileID).path) == "macrocall";
}

bool SourceManager::IsSourceFileExist(const unsigned int id)
{
    // Check whether the *.macrocall exists or not.
    if (sources.count(id)) {
        auto path = sources.at(id).path;
        if (!path.empty() && FileUtil::GetFileExtension(path) != "cj") {
            return FileUtil::FileExist(path);
        }
    }
    return true;
}

int SourceManager::GetLineEnd(const Position& pos)
{
    CJC_ASSERT(pos.line > 0);
    if (pos.line <= 0) {
        return 0;
    }

    auto found = sources.find(pos.fileID);
    if (found == sources.end()) {
        return 0;
    }

    auto buffer = found->second.buffer;
    auto sourceSplitted = Utils::SplitLines(buffer);
    if (pos.line > static_cast<int>(sourceSplitted.size())) {
        return 0;
    }

    return static_cast<int>(sourceSplitted[static_cast<size_t>(pos.line - 1)].size());
}

std::string SourceManager::GetContentBetween(
    const Position& begin, const Position& end, const std::string& importGenericContent) const
{
    return GetContentBetween(begin.fileID, begin, end, importGenericContent);
}

std::string SourceManager::GetContentBetween(
    unsigned int fileID, const Position& begin, const Position& end, const std::string& importGenericContent) const
{
    if (fileID == 0 || begin <= INVALID_POSITION || end <= INVALID_POSITION || end < begin) {
        return "";
    }

    CJC_ASSERT(INVALID_POSITION < begin && begin <= end);
    CJC_ASSERT(!sources.empty());
    auto& sourceWithFileID = sources.count(fileID) ? sources.at(fileID) : sources.at(0);

    // Use OwnedPtr for temporary Source to avoid mixed return types in ternary operator (? tempObj : ref).
    // This helps compiler optimization by having consistent pointer types
    OwnedPtr<Source> tempSource;
    Ptr<const Source> sourcePtr;

    if (sourceWithFileID.buffer.empty() && !importGenericContent.empty()) {
        tempSource = MakeOwned<Source>(sourceWithFileID.fileID, sourceWithFileID.path, importGenericContent);
        sourcePtr = tempSource.get();
    } else {
        sourcePtr = &sourceWithFileID;
    }

    const auto& buffer = sourcePtr->buffer;

    if (buffer.empty()) {
        return "";
    }

    auto startOffset = sourcePtr->PosToOffset(begin);
    auto endOffset = sourcePtr->PosToOffset(end);
    return buffer.substr(startOffset, endOffset - startOffset);
}

void SourceManager::AddComments(const TokenVecMap& commentsMap)
{
    for (const auto& it : commentsMap) {
        auto& source = sources.at(it.first);
        for (auto tok : it.second) {
            (void)source.offsetCommentsMap.insert_or_assign(source.PosToOffset(tok.Begin()), tok);
        }
    }
}

namespace Cangjie {
const std::string SourceManager::testPkgSuffix = "$test";
}
