// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the builtin operator's type helper functions for TypeCheck.
 */

#ifndef CANGJIE_SEMA_BUILTIN_OPERATOR_UTIL_H
#define CANGJIE_SEMA_BUILTIN_OPERATOR_UTIL_H

#include <map>

#include "cangjie/AST/Node.h"

namespace Cangjie::TypeCheckUtil {
/* Check whether given type(s) and operator kind is builtin operation. */
bool IsBuiltinUnaryExpr(TokenKind op, const AST::Ty& ty);
bool IsBuiltinBinaryExpr(TokenKind op, AST::Ty& leftTy, AST::Ty& rightTy);
bool IsUnaryOperator(TokenKind op);
bool IsBinaryOperator(TokenKind op);
AST::TypeKind GetBuiltinBinaryExprReturnKind(TokenKind op, AST::TypeKind leftOpType);
AST::TypeKind GetBuiltinUnaryOpReturnKind(TokenKind op, AST::TypeKind opType);
const std::map<AST::TypeKind, AST::TypeKind>& GetBinaryOpTypeCandidates(TokenKind op);
const std::map<AST::TypeKind, AST::TypeKind>& GetUnaryOpTypeCandidates(TokenKind op);
} // namespace Cangjie::TypeCheckUtil

#endif
