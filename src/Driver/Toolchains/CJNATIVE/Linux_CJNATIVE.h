// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Linux_CJNATIVE class.
 */

#ifndef CANGJIE_DRIVER_TOOLCHAIN_Linux_CJNATIVE_H
#define CANGJIE_DRIVER_TOOLCHAIN_Linux_CJNATIVE_H

#include "cangjie/Driver/Backend/Backend.h"
#include "cangjie/Driver/Driver.h"
#include "Toolchains/Gnu.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"

namespace Cangjie {
class Linux_CJNATIVE : public Gnu {
public:
    Linux_CJNATIVE(const Cangjie::Driver& driver, const DriverOptions& driverOptions,
        std::vector<ToolBatch>& backendCmds)
        : Gnu(driver, driverOptions, backendCmds) {};
    ~Linux_CJNATIVE() override = default;

protected:
    const std::vector<std::string> LINUX_CJNATIVE_LINK_OPTIONS = {
#define CJNATIVE_GNU_LINUX_BASIC_OPTIONS(OPTION) (OPTION),
#include "Toolchains/BackendOptions.inc"
#undef CJNATIVE_GNU_LINUX_BASIC_OPTIONS
    };
    const std::vector<std::string> LINUX_STATIC_LINK_OPTIONS = {
#define CJNATIVE_STATIC_LINK_BASIC_OPTIONS(OPTION) (OPTION),
#include "Toolchains/BackendOptions.inc"
#undef CJNATIVE_STATIC_LINK_BASIC_OPTIONS
    };
    // Gather library paths from LIBRARY_PATH and compiler guesses.
    void AddSystemLibraryPaths() override;
    // crtbeginS.o is used in place of crtbegin.o when generating PIEs.
    std::pair<std::string, std::string> GetGccCrtFilePair() const override;
    std::string GetCjldScript(const std::unique_ptr<Tool>& tool) const;
    void GenerateLinkingTool(const std::vector<TempFileInfo>& objFiles, const std::string& gccLibPath,
        const std::pair<std::string, std::string>& gccCrtFilePair) override;
    void GenerateLinkOptions(Tool& tool) override;
    void GenerateLinkOptionsForLTO(Tool& tool) const;
};
} // namespace Cangjie
#endif // CANGJIE_DRIVER_TOOLCHAIN_Linux_CJNATIVE_H
