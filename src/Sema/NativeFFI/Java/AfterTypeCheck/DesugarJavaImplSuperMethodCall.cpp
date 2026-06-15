// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "DesugarJavaImplSuperMethodCall.h"
#include "Utils.h"
#include "InteropLibBridge.h"

#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Types.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/Utils/SafePointer.h"
#include <utility>

namespace Cangjie::Native::FFI::Java {
using namespace Cangjie::Interop::Java;

void DesugarSuperMethodCallInJavaImplReferenceWrapper::DesugarSuperMethodCall(CallExpr& call,
    ClassDecl& impl) const
{
    CJC_ASSERT(call.baseFunc && call.baseFunc->astKind == ASTKind::MEMBER_ACCESS);
    auto& ma = *StaticAs<ASTKind::MEMBER_ACCESS>(call.baseFunc.get());
    CJC_ASSERT(ma.baseExpr && ma.baseExpr->astKind == ASTKind::REF_EXPR);
    auto& ref = *StaticAs<ASTKind::REF_EXPR>(ma.baseExpr.get().get());
    CJC_ASSERT(ref.isSuper);
    CJC_ASSERT(call.resolvedFunction && call.resolvedFunction->outerDecl);
    auto& outerDecl = *call.resolvedFunction->outerDecl;
    CJC_ASSERT(IsMirror(outerDecl));
    auto parent = As<ASTKind::CLASS_DECL>(&outerDecl);
    auto curFile = call.curFile;

    std::vector<OwnedPtr<Expr>> args;

    for (auto& arg : call.args) {
        auto desugaredArg = ilib.WrapJavaEntity(ASTCloner::Clone(arg->expr.get()));
        args.emplace_back(std::move(desugaredArg));
    }

    auto desugaredCall = ilib.CreateCallMethodCall(ilib.CreateGetJniEnvCall(curFile), CreateJavaRefCall(impl, curFile),
        MemberJNISignature(utils, *call.resolvedFunction, parent), std::move(args), *curFile, false);
    desugaredCall->desugarArgs = std::nullopt;

    call.desugarExpr = call.GetTy()->IsUnit()
        ? std::move(desugaredCall)
        : ilib.UnwrapJavaEntity(std::move(desugaredCall), call.GetTy(), impl);
}

DesugarSuperMethodCallInJavaImplReferenceWrapper::DesugarSuperMethodCallInJavaImplReferenceWrapper(
    JavaDesugarManager& man) : ilib(man.lib), utils(man.utils)
{}

void DesugarSuperMethodCallInJavaImplReferenceWrapper::Process(AfterTypeCheckContext& ctx)
{
    for (auto& jimpl : ctx.GetJavaImplReferenceWrappers()) {
        Walker(jimpl, [this, &jimpl](auto node) {
            if (node->TestAttr(Attribute::IS_BROKEN) || node->astKind == ASTKind::PRIMARY_CTOR_DECL) {
                return VisitAction::SKIP_CHILDREN;
            }
            auto call = As<ASTKind::CALL_EXPR>(node);
            if (!call) {
                return VisitAction::WALK_CHILDREN;
            }
            auto ma = As<ASTKind::MEMBER_ACCESS>(call->baseFunc.get());
            if (!ma || !ma->baseExpr) {
                return VisitAction::WALK_CHILDREN;
            }
            auto ref = As<ASTKind::REF_EXPR>(ma->baseExpr);
            if (!ref || !ref->isSuper) {
                return VisitAction::WALK_CHILDREN;
            }

            DesugarSuperMethodCall(*call, *StaticAs<ASTKind::CLASS_DECL>(jimpl));
            return VisitAction::WALK_CHILDREN;
        }).Walk();
    }
}

} // namespace Cangjie::Native::FFI::Java