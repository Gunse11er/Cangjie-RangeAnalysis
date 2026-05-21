// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Gnu class.
 */

#ifndef CANGJIE_DRIVER_TOOLCHAIN_GNU_H
#define CANGJIE_DRIVER_TOOLCHAIN_GNU_H

#include "cangjie/Driver/Backend/Backend.h"
#include "cangjie/Driver/Driver.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"

namespace Cangjie {

class Gnu : public ToolChain {
public:
    Gnu(const Cangjie::Driver& driver, const DriverOptions& driverOptions, std::vector<ToolBatch>& backendCmds)
        : ToolChain(driver, driverOptions, backendCmds) {};
    ~Gnu() override = default;

protected:
    std::string ldPath;
    std::string arPath;
    std::string objcopyPath;
    const std::pair<std::string, std::string> gccExecCrtFilePair = std::make_pair("crtbegin.o", "crtend.o");
    const std::pair<std::string, std::string> gccSharedCrtFilePair = std::make_pair("crtbeginS.o", "crtendS.o");

    std::string GetGccLibFile(const std::string& filename, const std::string& gccLibPath) const;
    // Get the executable formats of target system for the linker,
    // and the linker will emulate the target to do cross-compilation.
    virtual std::string GetEmulation() const;
    // Libc uses crtbegin.o/crtend.o to find the start of the constructors/destructors.
    virtual std::pair<std::string, std::string> GetGccCrtFilePair() const;

    bool PrepareDependencyPath() override;
    bool ProcessGeneration(std::vector<TempFileInfo>& objFiles) override;
    bool PerformPartialLinkAndContinue(std::vector<TempFileInfo>& objFiles);
    virtual std::string GenerateGCCLibPath(const std::pair<std::string, std::string>& gccCrtFilePair) const;

    virtual void GenerateArchiveTool(const std::vector<TempFileInfo>& objFiles);
    // utility method to find clang library, used to find asan and libfuzzer
    // clang library format: libclang_rt.<module name>[-<arch>].<suffix>
    std::optional<std::string> SearchClangLibrary(const std::string libName, const std::string libSuffix);
    void HandleSanitizer(Tool& tool, const std::string& cangjieLibPath, const std::string& gccLibPath);
    virtual void HandleSanitizerDependencies(Tool& tool);
    void HandleLLVMLinkOptions(const std::vector<TempFileInfo>& objFiles, const std::string& gccLibPath, Tool& tool,
        const std::string& cjldScript);
    virtual void HandleLibrarySearchPaths(Tool& tool, const std::string& cangjieLibPath);

    void InitializeLibraryPaths() override;
    virtual void AddCRuntimeLibraryPaths();
    // Gather library paths from LIBRARY_PATH and compiler guesses.
    virtual void AddSystemLibraryPaths();
    virtual void GenerateLinkOptions(Tool& tool)
    {
        (void)tool;
    }
    virtual void GenerateLinkingTool(const std::vector<TempFileInfo>& objFiles, const std::string& gccLibPath,
        const std::pair<std::string, std::string>& gccCrtFilePair)
    {
        (void)objFiles;
        (void)gccLibPath;
        (void)gccCrtFilePair;
    }
    virtual bool GenerateLinking(const std::vector<TempFileInfo>& objFiles);

private:
    void HandleAsanDependencies(Tool& tool, const std::string& cangjieLibPath, const std::string& gccLibPath);
    void HandleHwasanDependencies(Tool& tool, const std::string& cangjieLibPath);
    void GenericHandleSanitizerRuntime(
        Tool& tool, const std::string& name, const std::string& cangjieLibPath, std::function<void()> finalFallback);
};
} // namespace Cangjie
#endif // CANGJIE_DRIVER_TOOLCHAIN_GNU_H
