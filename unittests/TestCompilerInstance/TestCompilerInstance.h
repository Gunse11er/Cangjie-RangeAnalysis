// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the TestCompilerInstance, used for unittest.
 */

#ifndef CANGJIE_FRONTEND_TESTCOMPILERINSTANCE_H
#define CANGJIE_FRONTEND_TESTCOMPILERINSTANCE_H

#include "cangjie/Frontend/CompilerInstance.h"
#include "cangjie/Modules/PackageManager.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie {
class TestCompilerInstance : public CompilerInstance {
public:
    TestCompilerInstance(CompilerInvocation& invocation, DiagnosticEngine& diag) : CompilerInstance(invocation, diag)
    {
        compileOnePackageFromSrcFiles = false;
#ifdef PROJECT_SOURCE_DIR
        // Gets the absolute path of the project from the compile parameter.
        cangjieHome = FileUtil::JoinPath(FileUtil::JoinPath(PROJECT_SOURCE_DIR, "build"), "build");
#else
        // Just in case, give it a default value.
        // Assume the initial is in the build directory.
        cangjieHome = FileUtil::JoinPath(FileUtil::JoinPath(".", "build"), "build");
#endif
        // create modules dir.
        auto modulesName = FileUtil::JoinPath(cangjieHome, "modules");
        auto libPathName = invocation.globalOptions.GetCangjieLibTargetPathName();
        auto cangjieModules = FileUtil::JoinPath(modulesName, libPathName);
        if (!FileUtil::FileExist(cangjieModules)) {
            FileUtil::CreateDirs(FileUtil::JoinPath(cangjieModules, ""));
        }
        CJC_NULLPTR_CHECK(compileStrategy); // Was created in ctor of 'CompilerInstance'.
    }
    bool PerformParse() override;
    bool PerformSema() override
    {
        return compileStrategy->Sema();
    }
    bool Compile(CompileStage stage = CompileStage::GENERIC_INSTANTIATION) override;
    const auto& GetBuildOrders()
    {
        return packageManager->GetBuildOrders();
    }
    /**
     * Input source code.
     */
    std::string code;

private:
    bool ParseCode();
};
} // namespace Cangjie
#endif // CANGJIE_FRONTEND_TESTCOMPILERINSTANCE_H
