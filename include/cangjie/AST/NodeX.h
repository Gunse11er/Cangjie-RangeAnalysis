// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * AST nodes mainly includes eXtra AST nodes. These nodes are not available to Parser and libast, but visible to
 * semantic checkings. In other words, these nodes are pure semantic nodes.
 */

#ifndef CANGJIE_AST_NODEX_H
#define CANGJIE_AST_NODEX_H

#include "cangjie/AST/Node.h"

namespace Cangjie::AST {
/// @IfAvailable(name: arg, lambda1, lambda2) after macro expansion (before it is a MacroExpandExpr).
struct IfAvailableExpr : public Expr {
    IfAvailableExpr(OwnedPtr<FuncArg> namedArg, OwnedPtr<LambdaExpr> lambdaArg1, OwnedPtr<LambdaExpr> lambdaArg2)
        : Expr{ASTKind::IF_AVAILABLE_EXPR}, arg{std::move(namedArg)}, lambda1{std::move(lambdaArg1)},
        lambda2{std::move(lambdaArg2)}
    {
    }

    Ptr<FuncArg> GetArg() const
    {
        return arg.get();
    }
    Ptr<LambdaExpr> GetLambda1() const
    {
        return lambda1.get();
    }
    Ptr<LambdaExpr> GetLambda2() const
    {
        return lambda2.get();
    }

private:
    OwnedPtr<FuncArg> arg;
    OwnedPtr<LambdaExpr> lambda1;
    OwnedPtr<LambdaExpr> lambda2;
};

} // namespace Cangjie::AST
#endif
