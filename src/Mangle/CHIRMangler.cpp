// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the CHIRMangle.
 */

#include "cangjie/Mangle/CHIRMangler.h"

#include "cangjie/AST/Node.h"

using namespace Cangjie;
using namespace CHIR;
using namespace AST;
using namespace MangleUtils;


std::optional<std::string> CHIRMangler::MangleEntryFunction(const FuncDecl& funcDecl) const
{
    // Change user main function to user.main, so that the function entry can be changed to RuntimeMain.
    // Initialization of light-weight thread scheduling can be performed in runtime.main.
    if (enableCompileTest && funcDecl.identifier == TEST_ENTRY_NAME) {
        return USER_MAIN_MANGLED_NAME;
    }
    if (!enableCompileTest && (funcDecl.identifier == MAIN_INVOKE && funcDecl.IsStaticOrGlobal())) {
        return USER_MAIN_MANGLED_NAME;
    }
    return std::nullopt;
}

std::string CHIRMangler::MangleCFuncSignature(const AST::FuncTy& cFuncTy) const
{
    std::string mangled = MangleType(*cFuncTy.retTy);
    for (auto paramTy : cFuncTy.paramTys) {
        mangled += MangleType(*paramTy);
    }
    return mangled;
}
