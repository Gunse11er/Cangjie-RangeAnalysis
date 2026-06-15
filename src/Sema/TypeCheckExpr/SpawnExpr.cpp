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
        (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, se.arg.get())) &&
            CheckSpawnArgValid(ctx, *se.arg));
    CJC_NULLPTR_CHECK(se.task);
    isWellTyped = Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, se.task.get())) && isWellTyped;
    if (!isWellTyped) {
        se.SetTy(TypeManager::GetInvalidTy());
        return se.GetTy();
    }
    auto futureClass = importManager.GetCoreDecl<ClassDecl>("Future");
    if (futureClass == nullptr) {
        diag.Diagnose(se, DiagKind::sema_no_core_object);
        se.SetTy(TypeManager::GetInvalidTy());
        return se.GetTy();
    }
    CJC_ASSERT(Ty::IsTyCorrect(se.task->GetTy()) && se.task->GetTy()->IsFunc());
    CJC_ASSERT(!se.arg || (Ty::IsTyCorrect(se.arg->GetTy()) && se.arg->GetTy()->IsClassLike()));
    CJC_ASSERT(Ty::IsTyCorrect(futureClass->GetTy()) && futureClass->GetTy()->typeArgs.size() == 1);
    se.SetTy(typeManager.GetInstantiatedTy(futureClass->GetTy(),
        {{StaticCast<GenericsTy*>(futureClass->GetTy()->typeArgs.front()),
            RawStaticCast<FuncTy*>(se.task->GetTy())->retTy}}));
    return se.GetTy();
}

bool TypeChecker::TypeCheckerImpl::CheckSpawnArgValid(const ASTContext& ctx, const Expr& arg)
{
    CJC_ASSERT(arg.GetTy() != nullptr);
    CJC_ASSERT(Ty::IsTyCorrect(arg.GetTy()));
    // Check the ty of spawn argument is `ThreadContext` which from package `core`.
    auto threadContextInterface = importManager.GetCoreDecl<InterfaceDecl>("ThreadContext");
    if (threadContextInterface == nullptr) {
        (void)diag.Diagnose(arg, DiagKind::sema_no_core_object);
        return false;
    }
    if (arg.GetTy()->IsNothing() || !arg.GetTy()->IsClassLike() ||
        !typeManager.IsSubtype(arg.GetTy(), threadContextInterface->GetTy())) {
        DiagMismatchedTypes(diag, arg, *threadContextInterface->GetTy());
        return false;
    }
    // Check The spawn argument whether have `getSchedulerHandle` method,
    // whose signature is `()->CPointer<Unit>`. If not, just prompts that the type is invalid.
    auto classLikeTy = StaticCast<ClassLikeTy*>(arg.GetTy());
    auto retTy = typeManager.GetPointerTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    auto funcTy = typeManager.GetFunctionTy({}, retTy);
    CJC_NULLPTR_CHECK(arg.curFile);
    auto decls = FieldLookup(ctx, classLikeTy->commonDecl, "getSchedulerHandle", {.file = arg.curFile});
    if (decls.size() != 1 || decls[0]->astKind != ASTKind::FUNC_DECL ||
        !typeManager.IsSubtype(decls[0]->GetTy(), funcTy)) {
        (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_spawn_arg_invalid, arg);
        return false;
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::ChkSpawnExprSimple(ASTContext& ctx, Ty& tgtTy, SpawnExpr& se)
{
    if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, &se))) {
        if (typeManager.IsSubtype(se.GetTy(), &tgtTy)) {
            return true;
        } else {
            DiagMismatchedTypes(diag, se, tgtTy);
            se.SetTy(TypeManager::GetInvalidTy());
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
        auto fuTys = promotion.Downgrade(*futureClass->GetTy(), tgtTy);
        if (fuTys.empty()) {
            return ChkSpawnExprSimple(ctx, tgtTy, se);
        }
        fuTy = *fuTys.begin();
    }
    auto funcTy = typeManager.GetFunctionTy({}, fuTy->typeArgs.front());
    bool isWellTyped = !se.arg ||
        (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, se.arg.get())) &&
            CheckSpawnArgValid(ctx, *se.arg));
    isWellTyped = Check(ctx, funcTy, se.task.get()) && isWellTyped;
    if (!isWellTyped) {
        se.SetTy(TypeManager::GetInvalidTy());
        return false;
    }
    se.SetTy(fuTy);
    return true;
}
