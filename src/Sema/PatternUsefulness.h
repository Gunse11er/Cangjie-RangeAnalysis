// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Usefulness Checking related classes, which provides
 * exhaustiveness and reachability checking capabilities for pattern matching.
 */

#ifndef CANGJIE_SEMA_USEFULNESS_H
#define CANGJIE_SEMA_USEFULNESS_H

#include "cangjie/AST/Node.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie::PatternUsefulness {
bool CheckMatchExprHasSelectorExhaustivenessAndReachability(
    DiagnosticEngine& diag, TypeManager& typeManager, const AST::MatchExpr& me);
}

#endif
