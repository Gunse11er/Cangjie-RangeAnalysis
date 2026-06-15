// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/Basic/MacroCallDiagInfo.h"

#include "cangjie/Basic/DiagnosticEngine.h"

namespace Cangjie {

namespace {

/**
 * @brief Get the end position of a token in *.macrocall file by the same token's position in curfile.
 * @param key: the Hash value of line and column about token's end position in curfile.
 * @param first: the Hash value of line and column about token's begin position in curfile.
 * @param second: the begin position of the same token in *.macrocall file.
 * @return the end position in *.macrocall file.
 */
Position GetMacroCallEndPos(bool isCurFile, uint32_t key, uint32_t first, const Position second)
{
    auto end = second;
    if (!isCurFile) {
        // key and first are only column.
        auto columnOffset = (key > first) ? (key - first) : 0;
        return end + columnOffset;
    }
    // key and first are the Hash value of line and column that created by Position.Hash32().
    auto keyPos = Position::RestorePosFromHash(key);
    auto firstPos = Position::RestorePosFromHash(first);
    auto lineOffset = (keyPos.first > firstPos.first) ? (keyPos.first - firstPos.first) : 0;
    if (lineOffset > 0) {
        end.line += lineOffset;
        end.column = keyPos.second;
        return end;
    }
    auto columnOffset = (key > first) ? (key - first) : 0;
    return end + columnOffset;
}

/**
 * @brief Get the end position of a token for LSP using new2originPosMap.
 * @param newPos: the Hash value of line and column about token's end position in curfile.
 * @param first: the Hash value of line and column about token's begin position in curfile.
 * @return the end position in *.macrocall file.
 */
Position GetMacroCallEndPosforLsp(uint32_t newPos, uint32_t first, const Position second)
{
    auto end = second;
    // newPos and first are the Hash value of line and column that created by Position.Hash32().
    auto keyPos = Position::RestorePosFromHash(newPos);
    auto firstPos = Position::RestorePosFromHash(first);
    auto lineOffset = (keyPos.first > firstPos.first) ? (keyPos.first - firstPos.first) : 0;
    if (lineOffset > 0) {
        end.line += lineOffset;
        end.column = keyPos.second;
        return end;
    }
    auto columnOffset = (newPos > first) ? (newPos - first) : 0;
    return end + columnOffset;
}

} // namespace


Position MacroCallDiagInfo::MapPos(const Position& pos, bool isLowerBound) const
{
    if (IsCustomAnnotation()) {
        return pos;
    }
    if (macroCallBegin.fileID != pos.fileID) {
        // The original position and macrocall are not in the same file.
        return pos;
    }
    auto key = isCurFile ? pos.Hash32() : static_cast<uint32_t>(pos.column);
    if (isLowerBound) {
        if (isForLSP) {
            auto posIt = new2originPosMap.find(pos.Hash32());
            if (posIt != new2originPosMap.end()) {
                ++posIt;
            }
            if (posIt == new2originPosMap.end()) {
                posIt = new2originPosMap.lower_bound(pos.Hash32());
            }
            if (posIt == new2originPosMap.end()) {
                return pos;
            }
            auto sourcePos = GetMacroCallEndPosforLsp(pos.Hash32(), posIt->first, posIt->second);
            if (sourcePos.isCurFile) {
                return sourcePos;
            }
        }
        auto posIt = new2macroCallPosMap.lower_bound(key);
        if (posIt != new2macroCallPosMap.end()) {
            return GetMacroCallEndPos(isCurFile, key, posIt->first, posIt->second);
        }
        return pos;
    }
    // Get begin/identifier/field position.
    if (isForLSP) {
        if (new2originPosMap.find(pos.Hash32()) == new2originPosMap.end()) {
            return pos;
        }
        auto sourcePos = new2originPosMap.at(pos.Hash32());
        if (sourcePos.isCurFile) {
            return sourcePos;
        }
    }
    if (new2macroCallPosMap.find(key) != new2macroCallPosMap.end()) {
        return new2macroCallPosMap.at(key);
    }
    return pos;
}
} // namespace Cangjie
