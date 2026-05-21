// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"

using namespace Cangjie;
using namespace Sema;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynSpawnExpr(ASTContext& ctx, SpawnExpr& se)
{
    bool isWellTyped = !se.arg ||
        (Ty::IsTyCorrect(Synthesize(ctx, se.arg.get())) && CheckSpawnArgValid(ctx, *se.arg));
    CJC_NULLPTR_CHECK(se.task);
    isWellTyped = Ty::IsTyCorrect(Synthesize(ctx, se.task.get())) && isWellTyped;
    if (!isWellTyped) {
        se.ty = TypeManager::GetInvalidTy();
        return se.ty;
    }
    auto futureClass = importManager.GetCoreDecl<ClassDecl>("Future");
    if (futureClass == nullptr) {
        diag.Diagnose(se, DiagKind::sema_no_core_object);
        se.ty = TypeManager::GetInvalidTy();
        return se.ty;
    }
    CJC_ASSERT(Ty::IsTyCorrect(se.task->ty) && se.task->ty->IsFunc());
    CJC_ASSERT(!se.arg || (Ty::IsTyCorrect(se.arg->ty) && se.arg->ty->IsClassLike()));
    CJC_ASSERT(Ty::IsTyCorrect(futureClass->ty) && futureClass->ty->typeArgs.size() == 1);
    se.ty = typeManager.GetInstantiatedTy(futureClass->ty,
        {{StaticCast<GenericsTy*>(futureClass->ty->typeArgs.front()), RawStaticCast<FuncTy*>(se.task->ty)->retTy}});
    return se.ty;
}

bool TypeChecker::TypeCheckerImpl::CheckSpawnArgValid(const ASTContext& ctx, const Expr& arg)
{
    CJC_ASSERT(arg.ty != nullptr);
    CJC_ASSERT(Ty::IsTyCorrect(arg.ty));
    // Check the ty of spawn argument is `ThreadContext` which from package `core`.
    auto threadContextInterface = importManager.GetCoreDecl<InterfaceDecl>("ThreadContext");
    if (threadContextInterface == nullptr) {
        (void)diag.Diagnose(arg, DiagKind::sema_no_core_object);
        return false;
    }
    if (arg.ty->IsNothing() || !arg.ty->IsClassLike() ||
        !typeManager.IsSubtype(arg.ty, threadContextInterface->ty)) {
        DiagMismatchedTypes(diag, arg, *threadContextInterface->ty);
        return false;
    }
    // Check The spawn argument whether have `getSchedulerHandle` method,
    // whose signature is `()->CPointer<Unit>`. If not, just prompts that the type is invalid.
    auto classLikeTy = StaticCast<ClassLikeTy*>(arg.ty);
    auto retTy = typeManager.GetPointerTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    auto funcTy = typeManager.GetFunctionTy({}, retTy);
    CJC_NULLPTR_CHECK(arg.curFile);
    auto decls = FieldLookup(ctx, classLikeTy->commonDecl, "getSchedulerHandle", {.file = arg.curFile});
    if (decls.size() != 1 || decls[0]->astKind != ASTKind::FUNC_DECL ||
        !typeManager.IsSubtype(decls[0]->ty, funcTy)) {
        (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_spawn_arg_invalid, arg);
        return false;
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::ChkSpawnExprSimple(ASTContext& ctx, Ty& tgtTy, SpawnExpr& se)
{
    if (Ty::IsTyCorrect(Synthesize(ctx, &se))) {
        if (typeManager.IsSubtype(se.ty, &tgtTy)) {
            return true;
        } else {
            DiagMismatchedTypes(diag, se, tgtTy);
            se.ty = TypeManager::GetInvalidTy();
            return false;
        }
    }
    return false;
}

bool TypeChecker::TypeCheckerImpl::ChkSpawnExpr(ASTContext& ctx, Ty& tgtTy, SpawnExpr& se)
{
    Ptr<Ty> fuTy;
    if (TypeManager::IsCoreFutureType(tgtTy)) {
        fuTy = &tgtTy;
    } else {
        auto futureClass = importManager.GetCoreDecl<ClassDecl>("Future");
        if (futureClass == nullptr) {
            diag.Diagnose(se, DiagKind::sema_no_core_object);
            return false;
        }
        auto fuTys = promotion.Downgrade(*futureClass->ty, tgtTy);
        if (fuTys.empty()) {
            return ChkSpawnExprSimple(ctx, tgtTy, se);
        }
        fuTy = *fuTys.begin();
    }
    auto funcTy = typeManager.GetFunctionTy({}, fuTy->typeArgs.front());
    bool isWellTyped = !se.arg ||
        (Ty::IsTyCorrect(Synthesize(ctx, se.arg.get())) && CheckSpawnArgValid(ctx, *se.arg));
    isWellTyped = Check(ctx, funcTy, se.task.get()) && isWellTyped;
    if (!isWellTyped) {
        se.ty = TypeManager::GetInvalidTy();
        return false;
    }
    se.ty = fuTy;
    return true;
}
