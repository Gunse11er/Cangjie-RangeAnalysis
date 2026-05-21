// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements compiler's version apis.
 */

#include "cangjie/Basic/Print.h"
#include "cangjie/Basic/Version.h"

namespace Cangjie {
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
const std::string CANGJIE_VERSION = CJ_SDK_VERSION;

#ifndef VERSION_TAIL
const std::string CANGJIE_COMPILER_VERSION = "Cangjie Compiler: " + CANGJIE_VERSION;
#else
const std::string CANGJIE_COMPILER_VERSION = "Cangjie Compiler: " + CANGJIE_VERSION + VERSION_TAIL;
#endif
#endif
void PrintVersion()
{
    Cangjie::Println(CANGJIE_COMPILER_VERSION);
}
} // namespace Cangjie
