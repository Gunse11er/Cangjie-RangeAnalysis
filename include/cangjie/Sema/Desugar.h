// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Desugar function.
 */
#ifndef CANGJIE_SEMA_DESUGAR_H
#define CANGJIE_SEMA_DESUGAR_H

#include "cangjie/AST/Node.h"

namespace Cangjie {
using namespace AST;
void PerformDesugarBeforeTypeCheck(Node& root, bool desugarMacrocall = false);
} // namespace Cangjie

#endif
