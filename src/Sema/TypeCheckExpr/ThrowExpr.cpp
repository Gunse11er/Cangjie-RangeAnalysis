// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

using namespace Cangjie;
using namespace AST;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynThrowExpr(ASTContext& ctx, ThrowExpr& te)
{
    CJC_NULLPTR_CHECK(te.expr); // Parser guarantees.
    Synthesize(ctx, te.expr.get());
    te.ty = TypeManager::GetNothingTy();
    if (!Ty::IsTyCorrect(te.expr->ty)) {
        return TypeManager::GetInvalidTy();
    }
    if (te.expr->ty->IsEnum()) {
        if (auto refExpr = DynamicCast<RefExpr*>(te.expr.get()); refExpr && refExpr->ref.identifier == RESOURCE_NAME) {
            return TypeManager::GetNothingTy();
        }
    } else if (te.expr->ty->IsClass() || te.expr->ty->IsGeneric()) {
        // Check if the type of expression thrown is derived from `core.Exception` class
        // For class type and generic type.
        auto exception = importManager.GetCoreDecl<ClassDecl>(CLASS_EXCEPTION);
        auto error = importManager.GetCoreDecl<ClassDecl>(CLASS_ERROR);
        bool foundClass = exception && error;
        if (foundClass &&
            (typeManager.IsSubtype(te.expr->ty, exception->ty) || typeManager.IsSubtype(te.expr->ty, error->ty))) {
            return TypeManager::GetNothingTy();
        }
    }
    diag.Diagnose(te, DiagKind::sema_throw_expr_with_wrong_type);
    return TypeManager::GetInvalidTy();
}
