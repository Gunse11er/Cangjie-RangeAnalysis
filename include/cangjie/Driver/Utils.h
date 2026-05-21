// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares some utility functions.
 */

#ifndef CANGJIE_DRIVER_UTILS_H
#define CANGJIE_DRIVER_UTILS_H

#include <optional>
#include <string>
#include <vector>

namespace Cangjie {
/**
 * @brief Get the input string quoted with single quotes.
 * Note: Single quotes in the input are transform to '\'' instead of \'.
 *
 * @param str The input string.
 * @return std::string The single quoted string.
 */
std::string GetSingleQuoted(const std::string& str);

/**
 * @brief Get the input string quoted for passing as a command line argument.
 * - In the case of Linux, the argument is quoted with single quotes. Nested single quotes are
 *   transformed to '\''.
 * - In the case of Windows, the argument is quoted with double quotes. Nested double quotes and
 *   backslashes are escaped by \.
 *
 * @param arg The input string.
 * @return std::string The quoted argument.
 */
std::string GetCommandLineArgumentQuoted(const std::string& arg);

/**
 * @brief Prepend to paths.
 *
 * @param prefix The path prefix to be added.
 * @param paths The path vector.
 * @param quoted Determine whether the path string add single quotes.
 * @return std::vector<std::string> The vector of paths with a prefix added.
 */
std::vector<std::string> PrependToPaths(
    const std::string& prefix, const std::vector<std::string>& paths, bool quoted = false);

/**
 * @brief Get darwin SDK version.
 *
 * @param sdkPath The sdk path.
 * @return std::optional<std::string> The sdk version info.
 */
std::optional<std::string> GetDarwinSDKVersion(const std::string& sdkPath);
} // namespace Cangjie

#endif // CANGJIE_DRIVER_UTILS_H
