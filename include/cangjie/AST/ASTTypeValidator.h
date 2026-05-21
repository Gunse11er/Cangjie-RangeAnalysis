// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the class which used for validating correctness of ast sema ty.
 */

#ifndef CANGJIE_AST_TYPE_VALIDATOR_H
#define CANGJIE_AST_TYPE_VALIDATOR_H

#include "cangjie/AST/Node.h"
#include "cangjie/AST/PrintNode.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/AST/ASTCasting.h"

namespace Cangjie::AST {
void ValidateUsedNodes(DiagnosticEngine& diagEngine, Package& pkg);
} // namespace Cangjie::AST

#endif
