// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_TOSTRING_UTILS_H
#define CANGJIE_CHIR_TOSTRING_UTILS_H

#include "cangjie/CHIR/IR/IntrinsicKind.h"
#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

#include <cstddef>
#include <vector>

namespace Cangjie::CHIR {

std::string IndentToString(size_t indent);

/**
 * @brief Generates a string representation of generic constraints.
 *
 * @param genericTypeParams The list of generic type parameters.
 * @return A string representing the generic constraints.
 */
std::string GetGenericTypeConstaintsStr(const std::vector<GenericType*>& genericTypeParams);

/**
 * @brief Converts a package access level to a string.
 *
 * @param level The package access level to convert.
 * @return A string representing the package access level.
 */
std::string PackageAccessLevelToString(const Package::AccessLevel& level);
std::string OverflowToString(Cangjie::OverflowStrategy ofStrategy);

std::string StringJoin(const std::vector<std::string>& candidates, const std::string& delimiter);

template <typename T>
std::string ValueIdVecToString(const std::string& prefix,
    const std::vector<T*>& values, const std::string& suffix, bool hasExceptionBranch = false)
{
    if (values.empty()) {
        return "";
    }
    std::vector<std::string> ids;
    for (auto v : values) {
        if (v->IsLiteral()) {
            ids.emplace_back(v->ToString(0));
        } else {
            ids.emplace_back(v->GetIdentifier());
        }
    }
    if (hasExceptionBranch) {
        constexpr size_t normalIdx = 2;     // index from the end
        constexpr size_t exceptionIdx = 1;  // index from the end
        constexpr size_t branchSize = 2;
        CJC_ASSERT(ids.size() >= branchSize);
        ids[ids.size() - normalIdx] = "normal: " + ids[ids.size() - normalIdx];
        ids[ids.size() - exceptionIdx] = "exception: " + ids[ids.size() - exceptionIdx];
    }
    return prefix + StringJoin(ids, ", ") + suffix;
}

template <typename T>
std::string TypeVecToString(const std::string& prefix,
    const std::vector<T*>& types, const std::string& suffix, const std::string& delimiter = ", ")
{
    if (types.empty()) {
        return "";
    }
    std::vector<std::string> ids;
    for (auto v : types) {
        ids.emplace_back(v->ToString());
    }
    return prefix + StringJoin(ids, delimiter) + suffix;
}

std::string IntrinsicKindToString(const IntrinsicKind kind);
std::string AddNewLineOrNot(const std::string& message);
std::string CommentToString(const std::vector<std::string>& message);
std::string CommentToString(const std::string& message);
} // namespace Cangjie::CHIR

#endif // CANGJIE_CHIR_TOSTRING_UTILS_H
