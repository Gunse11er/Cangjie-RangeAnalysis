// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares GCCPath and GCCPathScanner.
 * Our toolchain uses some files under gcc library path to generate executable files.
 * We need to pass gcc library path to 'ld' command for linking these necessary .o files.
 * GCCPath stores gcc library path and gcc installation path.
 * GCCPathScanner automatically scans file system for an available gcc installation and return its path.
 * How GCCPathScanner scans depends on the triple target, i.e. cpu architecture, vendor and os.
 */

#ifndef CANGJIE_DRIVER_GCCPATHSCANNER_H
#define CANGJIE_DRIVER_GCCPATHSCANNER_H

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "cangjie/Option/Option.h"

namespace Cangjie {
// Version: major.minor.build
struct GCCVersion {
public:
    uint8_t major;
    uint8_t minor;
    uint8_t build;

    /**
     * @brief Compare GCC versions.
     *
     * @return bool Return true If GCC version number is larger.
     */
    bool operator<(const GCCVersion& gccVersion) const
    {
        if (major < gccVersion.major) {
            return true;
        } else if (major > gccVersion.major) {
            return false;
        }
        if (minor < gccVersion.minor) {
            return true;
        } else if (minor > gccVersion.minor) {
            return false;
        }
        return build < gccVersion.build;
    }
};

struct GCCPath {
public:
    std::string libPath;
    GCCVersion gccVersion;
};

/**
 * GCCPathScanner searches some default paths in system for an available gcc installation.
 */
class GCCPathScanner {
public:
    /**
     * @brief The constructor of class GCCPathScanner.
     *
     * @param tripleTarget The target format information.
     * @param files The crt files that need for linking.
     * @param searchPaths The default paths.
     * @return GCCPathScanner The instance of GCCPathScanner.
     */
    GCCPathScanner(const Triple::Info& tripleTarget, const std::vector<std::string>& files,
        const std::vector<std::string>& searchPaths)
        : tripleTarget(tripleTarget), forFiles(files), searchPaths(searchPaths){};

    /**
     * @brief The destructor of class GCCPathScanner.
     */
    ~GCCPathScanner() = default;

    /**
     * @brief Scan some possible directories where gcc may be installed.
     *
     * @return std::optional<GCCPath> The possible directories where gcc may be installed.
     */
    std::optional<GCCPath> Scan();

    /**
     * @brief To convert the string to GCCVersion.
     *
     * @param versionStr The version string.
     * @return std::optional<GCCVersion> The optional GCC version.
     */
    static std::optional<GCCVersion> StrToGCCVersion(const std::string& versionStr);
private:
    const Triple::Info tripleTarget;
    const std::vector<std::string> forFiles;
    std::vector<std::string> searchPaths;
    static std::vector<FileUtil::Directory> GetAllMatchingSubDirectories(
        const Triple::Info& target, const std::vector<FileUtil::Directory>& directories);
    bool AllForFilesExist(const std::string& path);
    static bool IsCompatiableTripleName(const Triple::Info& info, const std::string& name);
};
} // namespace Cangjie

#endif // CANGJIE_DRIVER_GCCPATHSCANNER_H
