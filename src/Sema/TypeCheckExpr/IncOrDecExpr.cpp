// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "DiagSuppressor.h"
#include "Diags.h"
#include "cangjie/AST/RecoverDesugar.h"

using namespace Cangjie;
using namespace AST;
using namespace Sema;

bool TypeChecker::TypeCheckerImpl::ChkIncOrDecExpr(ASTContext& ctx, Ty& target, IncOrDecExpr& ide)
{
    if (!Ty::IsTyCorrect(SynIncOrDecExpr(ctx, ide))) {
        return false;
    }
    if (typeManager.IsSubtype(ide.ty, &target)) {
        return true;
    }
    DiagMismatchedTypesWithFoundTy(diag, ide, target, *ide.ty, "the type of an assignment expression is always 'Unit'");
    ide.ty = TypeManager::GetInvalidTy();
    return false;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynIncOrDecExpr(ASTContext& ctx, IncOrDecExpr& ide)
{
    if (ide.desugarExpr == nullptr) { // `ide` or parent of `ide` is broken.
        return TypeManager::GetInvalidTy();
    }
    auto& ae = *StaticCast<AssignExpr*>(ide.desugarExpr.get());
    auto leftTy = Synthesize(ctx, ae.leftValue.get());
    if (!Ty::IsTyCorrect(leftTy)) {
        ide.ty = TypeManager::GetInvalidTy();
    } else if (!leftTy->IsInteger()) {
        DiagMismatchedTypesWithFoundTy(diag, *ae.leftValue, "integer type", leftTy->String(),
            "the base of increment or decrement expressions should be of integer type");
        ide.ty = TypeManager::GetInvalidTy();
    } else {
        if (ae.leftValue->astKind == ASTKind::SUBSCRIPT_EXPR && ae.leftValue->desugarExpr != nullptr) {
            RecoverToSubscriptExpr(StaticCast<SubscriptExpr&>(*ae.leftValue));
        }
        ide.ty = Synthesize(ctx, ide.desugarExpr.get());
    }
    return ide.ty;
}
