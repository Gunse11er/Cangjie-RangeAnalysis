// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

using namespace Cangjie;
using namespace AST;

// The function does not need a target type since bottom type is a subtype of all (target) types.
bool TypeChecker::TypeCheckerImpl::ChkReturnExpr(ASTContext& ctx, ReturnExpr& re)
{
    return Ty::IsTyCorrect(SynReturnExpr(ctx, re));
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynReturnExpr(ASTContext& ctx, ReturnExpr& re)
{
    if (!re.refFuncBody || !re.refFuncBody->retType) {
        re.ty = TypeManager::GetInvalidTy();
        return re.ty;
    }

    CJC_ASSERT(re.expr);
    bool isWellTyped = true;
    re.ty = TypeManager::GetInvalidTy();

    // Analyse re.expr.
    auto retTy = re.refFuncBody->retType->ty;
    if (Ty::IsTyCorrect(retTy) && !retTy->IsQuest()) {
        bool isInConstructor = re.refFuncBody->funcDecl && IsInstanceConstructor(*re.refFuncBody->funcDecl);
        if (isInConstructor) {
            isWellTyped = CheckReturnInConstructors(ctx, re) && isWellTyped;
        } else {
            isWellTyped = Check(ctx, retTy, re.expr.get());
        }
        if (isWellTyped) {
            ctx.targetTypeMap[re.expr.get()] = re.expr->ty;
        }
    } else {
        isWellTyped = Synthesize(ctx, re.expr.get()) && ReplaceIdealTy(*re.expr);
    }

    // Replace ClassThisTy to ClassTy when the function's outer declaration is not Class or Extend which extends class.
    if (!Is<ClassDecl>(re.refFuncBody->parentClassLike)) {
        if (auto ctt = DynamicCast<ClassThisTy*>(re.expr->ty); ctt && ctt->decl) {
            re.expr->ty = ctt->decl->ty;
        }
    }

    // Generic decls imported from foreign code and created by auto-sdk have no body, no need to check return.
    if (!isWellTyped && NeedCheckBodyReturn(*re.refFuncBody)) {
        re.ty = TypeManager::GetInvalidTy();
    } else {
        re.ty = TypeManager::GetNothingTy();
    }

    return re.ty;
}

bool TypeChecker::TypeCheckerImpl::CheckReturnInConstructors(ASTContext& ctx, const ReturnExpr& re)
{
    CJC_NULLPTR_CHECK(re.expr);
    return Check(ctx, TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT), re.expr.get());
}

bool TypeChecker::TypeCheckerImpl::NeedCheckBodyReturn(const FuncBody& fb) const
{
    if (fb.parentClassLike && HasJavaAttr(*fb.parentClassLike) &&
        fb.parentClassLike->TestAttr(Attribute::GENERIC, Attribute::IMPORTED)) {
        return false;
    }
    return !(fb.funcDecl && HasJavaAttr(*fb.funcDecl) &&
        fb.funcDecl->TestAttr(Attribute::GENERIC, Attribute::IMPORTED));
}
