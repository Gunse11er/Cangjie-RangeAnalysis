// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

using namespace Cangjie;
using namespace AST;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynParenExpr(ASTContext& ctx, ParenExpr& pe)
{
    (void)Synthesize(ctx, pe.expr.get());
    if (!pe.expr || !Ty::IsTyCorrect(pe.expr->ty)) {
        pe.ty = TypeManager::GetInvalidTy();
        return TypeManager::GetInvalidTy();
    }

    if (pe.expr->ty->IsIdeal()) {
        ReplaceIdealTy(*pe.expr);
    }
    pe.ty = pe.expr->ty;
    if (pe.expr->isConst) {
        pe.isConst = true;
        pe.constNumValue = pe.expr->constNumValue;
    }
    return pe.ty;
}

bool TypeChecker::TypeCheckerImpl::ChkParenExpr(ASTContext& ctx, Ty& target, ParenExpr& pe)
{
    if (Check(ctx, &target, pe.expr.get())) {
        CJC_NULLPTR_CHECK(pe.expr); // When the Check's result is true, pe.expr must not be nullptr.
        pe.ty = pe.expr->ty;
        if (pe.expr->isConst) {
            pe.isConst = true;
            pe.constNumValue = pe.expr->constNumValue;
        }
        return true;
    } else {
        pe.ty = TypeManager::GetInvalidTy();
        return false;
    }
}
