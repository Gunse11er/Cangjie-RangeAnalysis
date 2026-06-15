// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/RecoverDesugar.h"

using namespace Cangjie;
using namespace Sema;
using namespace TypeCheckUtil;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynTypeConvExpr(ASTContext& ctx, TypeConvExpr& tce)
{
    CJC_NULLPTR_CHECK(tce.expr);
    CJC_NULLPTR_CHECK(tce.type);
    Synthesize({ctx, SynPos::EXPR_ARG}, tce.expr.get());
    ReplaceIdealTy(*tce.expr);
    if (tce.type->astKind == ASTKind::PRIMITIVE_TYPE) {
        return SynNumTypeConvExpr(tce);
    }

    // The TypeConvExpr supports conversion between primitive types and conversion from CPointer to CFunc.
    // Therefore, the function should be returned in either of the above two branches.
    // Otherwise, there must be errors reported by other modules or logic codes.
    tce.SetTy(TypeManager::GetInvalidTy());
    return tce.GetTy();
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynNumTypeConvExpr(TypeConvExpr& tce)
{
    tce.SetTy(TypeManager::GetPrimitiveTy(StaticCast<PrimitiveType*>(tce.type.get())->kind));
    if (!Ty::IsTyCorrect(tce.expr->GetTy()) || !Ty::IsTyCorrect(tce.GetTy())) {
        tce.SetTy(TypeManager::GetInvalidTy());
        return tce.GetTy();
    }
    // Case 0: expr is of Nothing type, e.g., `UInt32(return)`
    bool isExprNothing =
        (tce.TyKind() == TypeKind::TYPE_RUNE || tce.GetTy()->IsNumeric()) && tce.expr->GetTy()->IsNothing();
    // Case 1: Rune to UInt32, e.g., `UInt32('a')`
    bool isRuneToUInt32 = tce.TyKind() == TypeKind::TYPE_UINT32 && tce.expr->TyKind() == TypeKind::TYPE_RUNE;
    // Case 2: Integer to Rune, e.g., `Rune(97)`
    bool isIntegerToChar = tce.TyKind() == TypeKind::TYPE_RUNE && tce.expr->GetTy()->IsInteger();
    // Case 3: convert between numeric types
    bool isBetweenNumeric = tce.GetTy()->IsNumeric() && tce.expr->GetTy()->IsNumeric();
    if (isExprNothing || isRuneToUInt32 || isIntegerToChar || isBetweenNumeric) {
        return tce.GetTy();
    }
    // Otherwise, return false.
    if (!CanSkipDiag(*tce.expr)) {
        diag.Diagnose(*tce.expr, DiagKind::sema_numeric_convert_must_be_numeric);
    }
    tce.SetTy(TypeManager::GetInvalidTy());
    return tce.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkTypeConvExpr(ASTContext& ctx, Ty& targetTy, TypeConvExpr& tce)
{
    // Additionally, given a context type T0 and an expression T1(t), since T1(t) : T1, we always require T1 <: T0.
    if (Ty::IsTyCorrect(SynTypeConvExpr(ctx, tce)) && typeManager.IsSubtype(tce.GetTy(), &targetTy)) {
        return true;
    } else {
        if (!CanSkipDiag(tce)) {
            DiagMismatchedTypes(diag, tce, targetTy);
        }
        tce.SetTy(TypeManager::GetInvalidTy());
        return false;
    }
}
