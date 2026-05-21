// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the cangjie compiler's version.
 */

#ifndef CANGJIE_DRIVER_VERSION_H
#define CANGJIE_DRIVER_VERSION_H

#include <string>

namespace Cangjie {
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
extern const std::string CANGJIE_VERSION;
extern const std::string CANGJIE_COMPILER_VERSION;
#endif
void PrintVersion();
} // namespace Cangjie
#endif // CANGJIE_DRIVER_VERSION_H
