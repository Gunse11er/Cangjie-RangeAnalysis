// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
* @file
*
* This is a file containing all functions that can recover ast node from desugar state.
*/

#ifndef CANGJIE_RECOVERDESUGAR_H
#define CANGJIE_RECOVERDESUGAR_H

#include "cangjie/AST/Node.h"

namespace Cangjie::AST {
void RecoverToBinaryExpr(BinaryExpr& be);
void RecoverToUnaryExpr(UnaryExpr& ue);
void RecoverToSubscriptExpr(SubscriptExpr& se);
void RecoverToAssignExpr(AssignExpr& ae);
void RecoverCallFromArrayExpr(CallExpr& ce);
void RecoverJArrayCtorCall(CallExpr& ce);
void RecoverToCallExpr(CallExpr& ce);
void RecoverFromVariadicCallExpr(CallExpr& ce);
}

#endif // CANGJIE_RECOVERDESUGAR_H
