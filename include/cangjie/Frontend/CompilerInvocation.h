// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the CompilerInvocation, which parses the arguments.
 */

#ifndef CANGJIE_FRONTEND_COMPILERINVOCATION_H
#define CANGJIE_FRONTEND_COMPILERINVOCATION_H

#include <memory>
#include <string>
#include <vector>

#include "cangjie/Frontend/FrontendOptions.h"
#include "cangjie/Option/Option.h"
#include "cangjie/Option/OptionTable.h"
#include "cangjie/Macro/InvokeUtil.h"

namespace Cangjie {
/**
 * All configuration for the compiler, options of all stages of translation.
 */
class CompilerInvocation {
public:
    CompilerInvocation()
    {
        optionTable = CreateOptionTable(true);
        argList = std::make_unique<ArgList>();
    }
    ~CompilerInvocation() = default;

    /**
     * Parse raw string arguments for compiler invocation.
     * @param args String representation of arguments.
     */
    bool ParseArgs(const std::vector<std::string>& args);
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    std::string GetRuntimeLibPath(const std::string& relativePath = "../runtime/lib");
#endif
    GlobalOptions globalOptions;
    FrontendOptions frontendOptions;
    std::unique_ptr<OptionTable> optionTable = nullptr;
    std::unique_ptr<ArgList> argList = nullptr;
};
}
#endif // CANGJIE_FRONTEND_COMPILERINVOCATION_H
