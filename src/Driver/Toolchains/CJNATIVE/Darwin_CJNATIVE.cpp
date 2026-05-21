// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the Darwin_CJNATIVE ToolChain base class.
 */

#include "Toolchains/CJNATIVE/Darwin_CJNATIVE.h"

#include "cangjie/Driver/TempFileManager.h"
#include "cangjie/Driver/ToolOptions.h"
#include "cangjie/Driver/Utils.h"
#include "cangjie/Utils/FileUtil.h"

using namespace Cangjie;
using namespace Cangjie::Triple;

void Darwin_CJNATIVE::AddSystemLibraryPaths()
{
    if (driverOptions.IsCrossCompiling() && driverOptions.customizedSysroot) {
        // user-specified sysroot is only considered in cross-compilation
        AddLibraryPaths(ComputeLibPaths());
    }
    MachO::AddSystemLibraryPaths();
}

TempFileInfo Darwin_CJNATIVE::GenerateLinkingTool(const std::vector<TempFileInfo>& objFiles,
    const std::string& darwinSDKVersion)
{
    auto tool = std::make_unique<Tool>(ldPath, ToolType::BACKEND, driverOptions.environment.allVariables);
    auto outputFileInfo = TempFileInfo{};
    if (driverOptions.stripSymbolTable) {
        TempFileKind kind = driverOptions.outputMode == GlobalOptions::OutputMode::SHARED_LIB
            ? TempFileKind::T_DYLIB_MAC
            : TempFileKind::T_EXE_MAC;
        outputFileInfo = CreateNewFileInfoWrapper(objFiles, kind);
    } else {
        outputFileInfo = GetOutputFileInfo(objFiles);
    }
    std::string outputFile = outputFileInfo.filePath;
    tool->AppendArg("-o", outputFile);
    tool->AppendArgIf(driverOptions.outputMode == GlobalOptions::OutputMode::SHARED_LIB, "-dylib");
    tool->AppendArg("-arch", GetTargetArchString());

    tool->AppendArg("-platform_version");
    tool->AppendArg("macos");
    tool->AppendArg("12.0.0");
    tool->AppendArg(darwinSDKVersion);

    tool->AppendArg("-syslibroot");
    tool->AppendArg(driverOptions.sysroot.empty() ? "/" : driverOptions.sysroot);

    if (driverOptions.stripSymbolTable) {
        tool->AppendArg("-install_name", GetOutputFileInfo(objFiles).filePath);
    }

    if (driverOptions.outputMode == GlobalOptions::OutputMode::EXECUTABLE) {
        tool->AppendArg("-pie");
    }
    HandleLLVMLinkOptions(objFiles, *tool);
    GenerateRuntimePath(*tool);
    backendCmds.emplace_back(MakeSingleToolBatch({std::move(tool)}));
    return outputFileInfo;
}

void Darwin_CJNATIVE::GenerateLinkOptions(Tool& tool)
{
    auto cangjieLibPath =
        FileUtil::JoinPath(FileUtil::JoinPath(driver.cangjieHome, "lib"), driverOptions.GetCangjieLibTargetPathName());
    if (driverOptions.linkStatic) {
        tool.AppendArg(FileUtil::JoinPath(cangjieLibPath, "libcangjie-runtime.a"));
        tool.AppendArg(FileUtil::JoinPath(cangjieLibPath, "libcangjie-thread.a"));
        tool.AppendArg(FileUtil::JoinPath(cangjieLibPath, "libboundscheck-static.a"));
        tool.AppendArg("-lc++");
    } else {
        tool.AppendArg("-lcangjie-runtime");
        tool.AppendArg("-lboundscheck");
    }
    tool.AppendArg("-lSystem");
    tool.AppendArg(FileUtil::JoinPath(cangjieLibPath, "libclang_rt.osx.a"));
}
