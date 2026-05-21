// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"

using namespace Cangjie;
using namespace Sema;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynOptionalChainExpr(ASTContext& ctx, OptionalChainExpr& oce)
{
    CJC_NULLPTR_CHECK(oce.desugarExpr);
    oce.ty = Synthesize(ctx, oce.desugarExpr.get());
    return oce.ty;
}

bool TypeChecker::TypeCheckerImpl::ChkOptionalChainExpr(ASTContext& ctx, Ty& target, OptionalChainExpr& oce)
{
    CJC_NULLPTR_CHECK(oce.desugarExpr);
    if (!Ty::IsTyCorrect(SynOptionalChainExpr(ctx, oce))) {
        return false;
    }
    if (!CheckOptionBox(target, *oce.desugarExpr->ty)) {
        DiagMismatchedTypes(diag, oce, target);
        oce.desugarExpr->ty = TypeManager::GetInvalidTy();
        oce.ty = TypeManager::GetInvalidTy();
        return false;
    }
    return true;
}
