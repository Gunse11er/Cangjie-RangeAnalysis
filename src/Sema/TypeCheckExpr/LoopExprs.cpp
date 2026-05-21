// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"
#include "TypeCheckUtil.h"

using namespace Cangjie;
using namespace Sema;
using namespace TypeCheckUtil;

namespace {
Ptr<Ty> GetIterableTy(TypeManager& tyMgr, ImportManager& importManager, Promotion& promotion, Ty& ty)
{
    // Promote implemented iterable type except nothing type.
    if (ty.IsNothing()) {
        return TypeManager::GetInvalidTy();
    }
    auto iterableInterface = importManager.GetCoreDecl("Iterable");
    if (auto genTy = DynamicCast<GenericsTy*>(&ty); genTy && genTy->isPlaceholder) {
        if (auto placeholderItTy = tyMgr.ConstrainByCtor(*genTy, *iterableInterface->ty)) {
            return placeholderItTy;
        } else {
            return TypeManager::GetInvalidTy();
        }
    }
    if (iterableInterface) {
        auto prTys = promotion.Promote(ty, *iterableInterface->ty);
        CJC_ASSERT(prTys.size() <= 1);
        return prTys.empty() ? TypeManager::GetInvalidTy() : *prTys.begin();
    }
    return TypeManager::GetInvalidTy();
}
} // namespace

bool TypeChecker::TypeCheckerImpl::ChkWhileExpr(ASTContext& ctx, Ty& target, WhileExpr& we)
{
    Ptr<Ty> unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    bool isWellTyped = typeManager.IsSubtype(unitTy, &target);
    if (!isWellTyped) {
        DiagMismatchedTypesWithFoundTy(diag, we, target, *unitTy);
    }
    isWellTyped = Ty::IsTyCorrect(SynWhileExpr(ctx, we)) && isWellTyped;
    return isWellTyped;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynWhileExpr(ASTContext& ctx, WhileExpr& we)
{
    bool isWellTyped = CheckCondition(ctx, *we.condExpr, false);
    isWellTyped = Ty::IsTyCorrect(Synthesize(ctx, we.body.get())) && isWellTyped;
    we.ty =
        isWellTyped ? StaticCast<Ty*>(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT)) : TypeManager::GetInvalidTy();
    return we.ty;
}

bool TypeChecker::TypeCheckerImpl::ChkDoWhileExpr(ASTContext& ctx, Ty& target, DoWhileExpr& dwe)
{
    Ptr<Ty> unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    bool isWellTyped = typeManager.IsSubtype(unitTy, &target);
    if (!isWellTyped) {
        DiagMismatchedTypesWithFoundTy(diag, dwe, target, *unitTy);
    }
    isWellTyped = Ty::IsTyCorrect(SynDoWhileExpr(ctx, dwe)) && isWellTyped;
    return isWellTyped;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynDoWhileExpr(ASTContext& ctx, DoWhileExpr& dwe)
{
    bool isWellTyped = Ty::IsTyCorrect(Synthesize(ctx, dwe.body.get()));
    isWellTyped = CheckCondition(ctx, *dwe.condExpr, false) && isWellTyped;
    dwe.ty =
        isWellTyped ? StaticCast<Ty*>(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT)) : TypeManager::GetInvalidTy();
    return dwe.ty;
}

bool TypeChecker::TypeCheckerImpl::ChkForInExpr(ASTContext& ctx, Ty& target, ForInExpr& fie)
{
    Ptr<Ty> unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    bool isWellTyped = typeManager.IsSubtype(unitTy, &target);
    if (!isWellTyped) {
        DiagMismatchedTypesWithFoundTy(diag, fie, target, *unitTy);
    }
    isWellTyped = Ty::IsTyCorrect(SynForInExpr(ctx, fie)) && isWellTyped;
    if (!isWellTyped) {
        fie.ty = TypeManager::GetInvalidTy();
    }
    return isWellTyped;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynForInExpr(ASTContext& ctx, ForInExpr& fie)
{
    CJC_NULLPTR_CHECK(fie.inExpression);
    CJC_NULLPTR_CHECK(fie.pattern);

    bool isWellTyped = Synthesize(ctx, fie.inExpression.get()) && ReplaceIdealTy(*fie.inExpression);

    // Implemented iterable in stdlib.
    CJC_NULLPTR_CHECK(fie.inExpression->ty);
    Ptr<Ty> iterableTy = GetIterableTy(typeManager, importManager, promotion, *fie.inExpression->ty);
    Ptr<Ty> inPatternTy = TypeManager::GetInvalidTy();
    if (Ty::IsTyCorrect(iterableTy)) {
        CJC_ASSERT(!iterableTy->typeArgs.empty());
        inPatternTy = iterableTy->typeArgs[0];
    } else {
        isWellTyped = false;
        if (!CanSkipDiag(*fie.inExpression)) {
            diag.Diagnose(
                *fie.inExpression, DiagKind::sema_expr_in_forin_must_has_iterator, Ty::ToString(fie.inExpression->ty));
        }
    }

    isWellTyped = Check(ctx, inPatternTy, fie.pattern.get()) && isWellTyped;
    if (fie.patternGuard) {
        // PatternGuard's ty should be boolean.
        if (!Check(ctx, TypeManager::GetPrimitiveTy(TypeKind::TYPE_BOOLEAN), fie.patternGuard.get())) {
            isWellTyped = false;
            if (!CanSkipDiag(*fie.patternGuard)) {
                diag.Diagnose(*fie.patternGuard, DiagKind::sema_wrong_forin_guard);
            }
        }
    }

    isWellTyped = Ty::IsTyCorrect(Synthesize(ctx, fie.body.get())) && isWellTyped;
    if (!IsIrrefutablePattern(*fie.pattern)) {
        isWellTyped = false;
        diag.Diagnose(fie, DiagKind::sema_forin_pattern_must_be_irrefutable);
    }

    fie.ty =
        isWellTyped ? StaticCast<Ty*>(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT)) : TypeManager::GetInvalidTy();
    return fie.ty;
}
