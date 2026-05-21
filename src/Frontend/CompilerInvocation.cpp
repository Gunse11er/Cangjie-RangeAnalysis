// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the CompilerInvocation.
 */

#include "cangjie/Frontend/CompilerInvocation.h"
#include "cangjie/Driver/Backend/CJNATIVEBackend.h"
#include "cangjie/Driver/DriverOptions.h"
#include "cangjie/Macro/InvokeUtil.h"

using namespace Cangjie;

bool CompilerInvocation::ParseArgs(const std::vector<std::string>& args)
{
    if (!optionTable->ParseArgs(args, *argList)) {
        return false;
    }

    frontendOptions.SetFrontendMode();
    if (!frontendOptions.ParseFromArgs(*argList)) {
        return false;
    }

    frontendOptions.SetCompilationCachedPath();
    globalOptions = frontendOptions;
    return true;
}

#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
std::string CompilerInvocation::GetRuntimeLibPath(const std::string& relativePath)
{
    auto runtimeLib = "libcangjie-runtime.so";
#ifdef _WIN64
    runtimeLib = "libcangjie-runtime.dll";
#elif defined(__APPLE__)
    runtimeLib = "libcangjie-runtime.dylib";
#endif
    std::string& exePath = globalOptions.executablePath;
    std::string hostPathName = globalOptions.GetCangjieLibHostPathName();
    auto basePath = FileUtil::JoinPath(FileUtil::GetDirPath(exePath), relativePath);
    auto runtimeLibPath =
        FileUtil::JoinPath(FileUtil::JoinPath(basePath, hostPathName), runtimeLib);
    return runtimeLibPath;
}
#endif