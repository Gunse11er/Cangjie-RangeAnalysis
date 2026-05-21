// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the map of standard library names.
 */

#ifndef CANGJIE_DIRVIER_STDLIBMAP_H
#define CANGJIE_DIRVIER_STDLIBMAP_H

#include <string>
#include <unordered_map>

namespace Cangjie {
const std::string GET_COMMAND_LINE_ARGS = "getCommandLineArgs";
const std::string MODULE_SPLIT = "/";

const std::unordered_map<std::string, std::string> STANDARD_LIBS = {
#define STDLIB(NAME, MODULE, SUB_PACKAGE) {MODULE "." SUB_PACKAGE, SUB_PACKAGE},
#define STDLIB_ROOTPKG(NAME, ROOT_PACKAGE) {ROOT_PACKAGE, ROOT_PACKAGE},
#define STDLIB_TOPPKG(NAME, TOP_PACKAGE) {TOP_PACKAGE, TOP_PACKAGE},
#include "cangjie/Driver/Stdlib.inc"
#undef STDLIB_TOPPKG
#undef STDLIB_ROOTPKG
#undef STDLIB
};
} // namespace Cangjie
#endif
