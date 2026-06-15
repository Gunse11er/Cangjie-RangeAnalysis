// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the APILevelVersion type methods.
 */

#include "APILevelVersion.h"

#include <optional>
#include <vector>

#include "cangjie/Utils/StdUtils.h"

namespace Cangjie {
namespace {
constexpr uint32_t APILEVEL_MAJOR_MIN = 1;
constexpr uint32_t APILEVEL_MAJOR_MAX = 99;
constexpr uint32_t APILEVEL_COMPONENT_MAX = 99;
constexpr size_t MAX_VERSION_PARTS = 3;
constexpr size_t TRIPLE_PARTS_COUNT = 3;
constexpr size_t PATCH_INDEX = 2;

std::vector<std::string> SplitVersionString(const std::string& s)
{
    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = s.find('.');
    while (end != std::string::npos) {
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
        end = s.find('.', start);
    }
    parts.push_back(s.substr(start));
    return parts;
}

bool IsDecimalComponent(const std::string& part)
{
    if (part.empty()) {
        return false;
    }
    if (part.size() > 1 && part[0] == '0') {
        return false;
    }
    for (char ch : part) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

bool CheckComponentRange(const std::string& part, uint32_t min, uint32_t max)
{
    if (!IsDecimalComponent(part)) {
        return false;
    }
    auto value = Stoull(part);
    return value.has_value() && value.value() >= min && value.value() <= max;
}
} // namespace

std::string APILevelVersion::ToDisplayString() const
{
    if (patch != 0) {
        return ToString();
    }
    if (minor != 0) {
        return std::to_string(major) + "." + std::to_string(minor);
    }
    return std::to_string(major);
}

APILevelVersion APILevelVersion::Parse(const std::string& s)
{
    constexpr size_t versionIdxMinor = 1;
    constexpr size_t versionIdxPatch = 2;
    constexpr size_t versionMinPartsMinor = 2;
    constexpr size_t versionMinPartsPatch = 3;

    APILevelVersion version;
    if (s.empty()) {
        return version;
    }

    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = s.find('.');
    while (end != std::string::npos) {
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
        end = s.find('.', start);
    }
    parts.push_back(s.substr(start));

    if (!parts[0].empty()) {
        version.major = static_cast<uint32_t>(Stoull(parts[0]).value_or(0));
    }
    if (parts.size() >= versionMinPartsMinor && !parts[versionIdxMinor].empty()) {
        version.minor = static_cast<uint32_t>(Stoull(parts[versionIdxMinor]).value_or(0));
    }
    if (parts.size() >= versionMinPartsPatch && !parts[versionIdxPatch].empty()) {
        version.patch = static_cast<uint32_t>(Stoull(parts[versionIdxPatch]).value_or(0));
    }

    return version;
}

bool APILevelVersion::IsValidFormat(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    size_t partCount = 0;
    size_t pos = 0;
    const size_t len = s.size();
    while (pos < len) {
        size_t dotPos = s.find('.', pos);
        size_t partEnd = (dotPos == std::string::npos) ? len : dotPos;
        if (partEnd == pos) {
            return false; // empty component (leading or consecutive dot)
        }
        for (size_t i = pos; i < partEnd; ++i) {
            if (s[i] < '0' || s[i] > '9') {
                return false;
            }
        }
        ++partCount;
        if (partCount > MAX_VERSION_PARTS) {
            return false;
        }
        if (dotPos == std::string::npos) {
            pos = len; // last part processed, exit loop
        } else {
            pos = dotPos + 1;
            if (pos == len) {
                return false; // trailing dot (e.g., "1.2.")
            }
        }
    }
    return partCount > 0;
}

std::optional<APILevelVersion> APILevelVersion::ParseChecked(const std::string& s, ParseRule rule)
{
    if (!IsValidFormat(s, rule)) {
        return std::nullopt;
    }
    return Parse(s);
}

bool APILevelVersion::IsValidFormat(const std::string& s, ParseRule rule)
{
    if (s.empty()) {
        return false;
    }
    auto parts = SplitVersionString(s);
    if (parts.size() == 1) {
        if (rule == ParseRule::TRIPLE_ONLY) {
            return false;
        }
        return CheckComponentRange(parts[0], APILEVEL_MAJOR_MIN, APILEVEL_MAJOR_MAX);
    }
    if (parts.size() != TRIPLE_PARTS_COUNT || rule == ParseRule::MAJOR_ONLY) {
        return false;
    }
    return CheckComponentRange(parts[0], APILEVEL_MAJOR_MIN, APILEVEL_MAJOR_MAX) &&
        CheckComponentRange(parts[1], 0, APILEVEL_COMPONENT_MAX) &&
        CheckComponentRange(parts[PATCH_INDEX], 0, APILEVEL_COMPONENT_MAX);
}

} // namespace Cangjie
