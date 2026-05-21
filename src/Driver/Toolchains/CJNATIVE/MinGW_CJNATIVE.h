// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the MinGW_CJNATIVE class.
 */

#ifndef CANGJIE_DRIVER_TOOLCHAIN_MINGW_CJNATIVE_H
#define CANGJIE_DRIVER_TOOLCHAIN_MINGW_CJNATIVE_H

#include "cangjie/Driver/Backend/Backend.h"
#include "cangjie/Driver/Driver.h"
#include "Toolchains/Gnu.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"

namespace Cangjie {
class MinGW_CJNATIVE : public Gnu {
public:
    MinGW_CJNATIVE(const Cangjie::Driver& driver, const DriverOptions& driverOptions,
        std::vector<ToolBatch>& backendCmds)
        : Gnu(driver, driverOptions, backendCmds)
    {
        mingwLibPath = FileUtil::JoinPath(driver.cangjieHome, "third_party/mingw/lib/");
    };
    ~MinGW_CJNATIVE() override = default;

protected:
    std::string GetSharedLibraryExtension() const override
    {
        return ".dll";
    }

    void InitializeLibraryPaths() override;

    void InitializeMinGWSysroot();

    void AddCRuntimeLibraryPaths() override;
    bool PrepareDependencyPath() override;

    // Gather library paths from LIBRARY_PATH and compiler guesses.
    void AddSystemLibraryPaths() override;

    std::vector<std::string> ComputeLibPaths() const override;

    std::string GenerateGCCLibPath(const std::pair<std::string, std::string>& gccCrtFilePair) const override;

    void GenerateArchiveTool(const std::vector<TempFileInfo>& objFiles) override;

    void HandleLibrarySearchPaths(Tool& tool, const std::string& cangjieLibPath) override;

    std::pair<std::string, std::string> GetGccCrtFilePair() const override
    {
        return gccExecCrtFilePair;
    }

    std::string GetCjldScript(const std::unique_ptr<Tool>& tool) const;
    void GenerateLinkingTool(const std::vector<TempFileInfo>& objFiles, const std::string& gccLibPath,
        const std::pair<std::string, std::string>& gccCrtFilePair) override;

    void GenerateLinkOptions(Tool& tool) override;

    std::string FindCangjieMinGWToolPath(const std::string toolName) const;

private:
    const std::vector<std::string> MINGW_CJNATIVE_LINK_OPTIONS = {
#define CJNATIVE_WINDOWS_BASIC_OPTIONS(OPTION) (OPTION),
#include "Toolchains/BackendOptions.inc"
#undef CJNATIVE_WINDOWS_BASIC_OPTIONS
    };
    std::string sysroot;
    std::string mingwLibPath;
};
} // namespace Cangjie

#endif // CANGJIE_DRIVER_TOOLCHAIN_MINGW_CJNATIVE_H
