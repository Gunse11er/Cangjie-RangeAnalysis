// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"

using namespace Cangjie;
using namespace Sema;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynIsExpr(ASTContext& ctx, IsExpr& ie)
{
    if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, ie.leftExpr.get())) &&
        Ty::IsTyCorrect(Synthesize({ctx, SynPos::NONE}, ie.isType.get())) && ReplaceIdealTy(*ie.leftExpr)) {
        ie.SetTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_BOOLEAN));
    } else {
        ie.SetTy(TypeManager::GetInvalidTy());
    }
    return ie.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkIsExpr(ASTContext& ctx, Ty& target, IsExpr& ie)
{
    // Always type checking the expression even if the target type mismatches.
    auto ty = SynIsExpr(ctx, ie);
    if (!Ty::IsTyCorrect(ty)) {
        return false;
    }
    bool isWellTyped = ty->IsBoolean();

    auto boolTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_BOOLEAN);
    if (!typeManager.IsLitBoxableType(boolTy, &target)) {
        DiagMismatchedTypesWithFoundTy(diag, ie, target, *boolTy);
        isWellTyped = false;
    }

    ie.SetTy(isWellTyped ? ie.GetTy() : TypeManager::GetInvalidTy());
    return isWellTyped;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynAsExpr(ASTContext& ctx, AsExpr& ae)
{
    if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, ae.leftExpr.get())) &&
        Ty::IsTyCorrect(Synthesize({ctx, SynPos::NONE}, ae.asType.get())) && ReplaceIdealTy(*ae.leftExpr)) {
        auto optionDecl = RawStaticCast<EnumDecl*>(importManager.GetCoreDecl("Option"));
        if (optionDecl) {
            ae.SetTy(typeManager.GetEnumTy(*optionDecl, {ae.asType->GetTy()}));
        } else {
            diag.Diagnose(ae, DiagKind::sema_no_core_object);
            ae.SetTy(TypeManager::GetInvalidTy());
        }
    } else {
        ae.SetTy(TypeManager::GetInvalidTy());
    }
    return ae.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkAsExpr(ASTContext& ctx, Ty& target, AsExpr& ae)
{
    if (!Ty::IsTyCorrect(SynAsExpr(ctx, ae))) {
        return false;
    }
    if (!CheckOptionBox(target, *ae.GetTy())) {
        DiagMismatchedTypes(diag, ae, target);
        ae.SetTy(TypeManager::GetInvalidTy());
        return false;
    }
    return true;
}
