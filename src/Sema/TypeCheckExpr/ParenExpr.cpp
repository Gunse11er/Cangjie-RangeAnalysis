// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

using namespace Cangjie;
using namespace AST;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynParenExpr(const CheckerContext& ctx, ParenExpr& pe)
{
    Synthesize(ctx, pe.expr.get());
    if (!pe.expr || !Ty::IsTyCorrect(pe.expr->GetTy())) {
        pe.SetTy(TypeManager::GetInvalidTy());
        return TypeManager::GetInvalidTy();
    }

    if (pe.expr->GetTy()->IsIdeal()) {
        ReplaceIdealTy(*pe.expr);
    }
    pe.SetTy(pe.expr->GetTy());
    if (pe.expr->isConst) {
        pe.isConst = true;
        pe.constNumValue = pe.expr->constNumValue;
    }
    return pe.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkParenExpr(ASTContext& ctx, Ty& target, ParenExpr& pe)
{
    if (Check(ctx, &target, pe.expr.get())) {
        CJC_NULLPTR_CHECK(pe.expr); // When the Check's result is true, pe.expr must not be nullptr.
        pe.SetTy(pe.expr->GetTy());
        if (pe.expr->isConst) {
            pe.isConst = true;
            pe.constNumValue = pe.expr->constNumValue;
        }
        return true;
    } else {
        pe.SetTy(TypeManager::GetInvalidTy());
        return false;
    }
}
