// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements desugaring of @ObjCImpl.
 */

#include "Context.h"
#include "Handlers.h"
#include "NativeFFI/ObjC/Utils/ASTFactory.h"
#include "NativeFFI/ObjC/Utils/Common.h"
#include "NativeFFI/Utils.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Walker.h"

using namespace Cangjie::AST;
using namespace Cangjie::Native::FFI;
namespace Cangjie::Interop::ObjC {

void DesugarImpls::HandleImpl(InteropContext& ctx)
{
    for (auto& impl : ctx.impls) {
        if (impl->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }

        for (auto& memberDecl : impl->GetMemberDeclPtrs()) {
            if (memberDecl->TestAttr(Attribute::IS_BROKEN)) {
                continue;
            }

            switch (memberDecl->astKind) {
                case ASTKind::FUNC_DECL: {
                    auto& fd = *StaticAs<ASTKind::FUNC_DECL>(memberDecl);
                    Desugar(ctx, *impl, fd);
                    break;
                }
                case ASTKind::PROP_DECL: {
                    Desugar(ctx, *impl, *StaticAs<ASTKind::PROP_DECL>(memberDecl));
                    break;
                }
                default:
                    break;
            }
        }
    }
}

namespace {
void DesugarSuperCtorCall(InteropContext& ctx, ClassDecl& impl, FuncDecl& ctor)
{
    // super(...) can appear only in constructors
    if (!ctor.TestAttr(Attribute::CONSTRUCTOR)) {
        return;
    }
    auto& body = ctor.funcBody->body->body;
    if (body.size() < 1) {
        return;
    }
    auto& firstExpr = body[0];
    auto ce = As<ASTKind::CALL_EXPR>(firstExpr);
    if (!ce || !ce->resolvedFunction || !IsSuperConstructorCall(*ce)) {
        return;
    }
    /**
     * super(...args)
     * -->
     * if doesn't have @ObjCImpl super class:
     * super({
     *  self = [Impl alloc]; // skipped, if `self` is already provided
     *  self = [super init:...args];
     *  [self setRegistryId:putToRegistry(This)];
     *  self
     * }, ...args)
     *
     * else if has @ObjCImpl super class:
     * super({
     *   self = [Impl alloc]; // skipped, if `self` is already provided
     * }, ...args)
     */
    OwnedPtr<Expr> objCSelf;
    auto isInGeneratedCtor = ctx.factory.IsGeneratedCtor(ctor);
    auto curFile = ce->curFile;
    auto targetFd = ce->resolvedFunction;
    if (isInGeneratedCtor) {
        // already have a self ptr
        auto& methodParams = ctor.funcBody->paramLists[0]->params;
        CJC_ASSERT(methodParams.size() > 0);
        objCSelf = WithinFile(CreateRefExpr(*methodParams[0]), curFile);
    } else {
        // alloc a new ptr
        objCSelf = ctx.factory.CreateAllocCall(impl, curFile);
    }

    if (HasImplSuperClass(impl)) {
        std::vector<OwnedPtr<FuncArg>> args;
        args.push_back(CreateFuncArg(std::move(objCSelf)));
        args.insert(args.end(), std::make_move_iterator(ce->args.begin()), std::make_move_iterator(ce->args.end()));

        auto realTarget = ctx.factory.GetGeneratedImplCtor(*GetImplSuperClass(impl), *targetFd);
        auto realTargetTy = StaticCast<FuncTy>(realTarget->GetTy());
        auto superCall = CreateSuperCall(*realTarget->outerDecl, *realTarget, realTargetTy);
        superCall->args = std::move(args);
        ce->desugarExpr = std::move(superCall);

        return;
    }

    auto withMethodEnv = WithinFile(
        ctx.factory.CreateWithMethodEnvScope(std::move(objCSelf), impl, impl.GetTy(),
            [&](auto&& receiver, auto&& objCSuper) {
                std::vector<OwnedPtr<Expr>> superInitArgs;
                std::transform(ce->args.begin(), ce->args.end(), std::back_inserter(superInitArgs), [&](auto& arg) {
                    return ctx.factory.UnwrapEntity(WithinFile(ASTCloner::Clone(arg->expr.get()), curFile));
                });
                auto superInit = ctx.factory.CreateMethodCallViaMsgSendSuper(
                    *targetFd, std::move(receiver), std::move(objCSuper), std::move(superInitArgs));

                auto tmpSelf = WithinFile(CreateTmpVarDecl(nullptr, std::move(superInit)), curFile);
                auto selfRef = WithinFile(CreateRefExpr(*tmpSelf), curFile);
                auto putToRegistry =
                    ctx.factory.CreatePutToRegistryCall(CreateThisRef(Ptr(&impl), impl.GetTy(), curFile));
                auto setRegistryId =
                    ctx.factory.CreateObjCMsgSendCall(ASTCloner::Clone(selfRef.get()), REGISTRY_ID_SETTER_SELECTOR,
                        TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT), Nodes<Expr>(std::move(putToRegistry)));

                return Nodes<Node>(std::move(tmpSelf), std::move(setRegistryId), std::move(selfRef));
            }),
        curFile);

    auto baseCtor = ctx.factory.GetGeneratedBaseCtor(impl);
    CJC_NULLPTR_CHECK(baseCtor);
    auto baseCtorCall = WithinFile(CreateSuperCall(*baseCtor->outerDecl, *baseCtor, baseCtor->GetTy()), curFile);
    baseCtorCall->args.push_back(CreateFuncArg(std::move(withMethodEnv)));
    ce->desugarExpr = std::move(baseCtorCall);
}

void DesugarThisCtorCall(InteropContext& ctx, ClassDecl& impl, FuncDecl& ctor)
{
    // this(...) can appear only in constructors
    if (!ctor.TestAttr(Attribute::CONSTRUCTOR)) {
        return;
    }
    auto& body = ctor.funcBody->body->body;
    if (body.size() < 1) {
        return;
    }
    auto& firstExpr = body[0];
    auto ce = As<ASTKind::CALL_EXPR>(firstExpr);
    if (!ce || !ce->resolvedFunction || !IsThisConstructorCall(*ce)) {
        return;
    }
    auto isInGeneratedCtor = ctx.factory.IsGeneratedCtor(ctor);
    if (!isInGeneratedCtor) {
        return;
    }
    /**
     * this(...args)
     * -->
     * if is in generated ctor:
     * this($obj, ...args)
     */
     auto curFile = ce->curFile;
     auto targetFd = ce->resolvedFunction;
     auto& methodParams = ctor.funcBody->paramLists[0]->params;
     CJC_ASSERT(methodParams.size() > 0);
     auto objCSelf = CreateRefExpr(*methodParams[0]);

     std::vector<OwnedPtr<FuncArg>> args;
     args.push_back(CreateFuncArg(std::move(objCSelf)));
     args.insert(args.end(), std::make_move_iterator(ce->args.begin()), std::make_move_iterator(ce->args.end()));

     auto realTarget = ctx.factory.GetGeneratedImplCtor(impl, *targetFd);
     auto realTargetTy = StaticCast<FuncTy>(realTarget->GetTy());
     ce->desugarExpr = CreateThisCall(impl, *realTarget, realTargetTy, curFile, std::move(args));
}
} // namespace

void DesugarImpls::Desugar(InteropContext& ctx, ClassDecl& impl, FuncDecl& method)
{
    DesugarSuperCtorCall(ctx, impl, method);
    DesugarThisCtorCall(ctx, impl, method);
    // We are interested in:
    // 1. CallExpr to MemberAccess, as it could be `super.<member>(...)`
    // 2. MemberAccess, as it could be a prop getter call
    Walker(method.funcBody->body.get(), [&](auto node) {
        if (node->TestAnyAttr(
                Attribute::HAS_BROKEN, Attribute::IS_BROKEN, Attribute::UNREACHABLE, Attribute::LEFT_VALUE)) {
            return VisitAction::SKIP_CHILDREN;
        }

        switch (node->astKind) {
            case ASTKind::CALL_EXPR:
                DesugarCallExpr(ctx, impl, *StaticAs<ASTKind::CALL_EXPR>(node));
                break;
            case ASTKind::MEMBER_ACCESS:
                DesugarGetForPropDecl(ctx, impl, *StaticAs<ASTKind::MEMBER_ACCESS>(node));
                break;
            default:
                break;
        }

        return VisitAction::WALK_CHILDREN;
    }).Walk();
}

void DesugarImpls::Desugar(InteropContext& ctx, ClassDecl& impl, PropDecl& prop)
{
    for (auto& getter : prop.getters) {
        Desugar(ctx, impl, *getter.get());
    }

    for (auto& setter : prop.setters) {
        Desugar(ctx, impl, *setter.get());
    }
}

void DesugarImpls::DesugarCallExpr(InteropContext& ctx, ClassDecl& impl, CallExpr& ce)
{
    if (ce.desugarExpr || !ce.baseFunc || !ce.resolvedFunction) {
        return;
    }

    if (ce.callKind != CallKind::CALL_SUPER_FUNCTION) {
        return;
    }

    auto targetFd = ce.resolvedFunction;
    if (targetFd->propDecl && targetFd->propDecl->TestAttr(Attribute::DESUGARED_MIRROR_FIELD)) {
        return;
    }

    if (ctx.factory.IsGeneratedCtor(*targetFd)) {
        return;
    }

    auto targetFdTy = StaticCast<FuncTy>(targetFd->GetTy());
    auto curFile = ce.curFile;

    // method/prop branch
    if (!ctx.typeMapper.IsObjCMirror(*targetFd->outerDecl->GetTy())) {
        // no need to desugar expr, if the target is not in the @ObjCMirror declaration
        return;
    }

    std::vector<OwnedPtr<Expr>> msgSendSuperArgs;
    std::transform(ce.args.begin(), ce.args.end(), std::back_inserter(msgSendSuperArgs),
        [&](auto& arg) { return ctx.factory.UnwrapEntity(WithinFile(ASTCloner::Clone(arg->expr.get()), curFile)); });

    auto nativeHandle = ctx.factory.CreateNativeHandleExpr(impl, false, ce.curFile);
    auto withMethodEnvCall = ctx.factory.CreateWithMethodEnvScope(
        std::move(nativeHandle), impl, targetFdTy->retTy, [&](auto&& receiver, auto&& objCSuper) {
            OwnedPtr<Expr> msgSendSuperCall;
            if (targetFd->propDecl) {
                if (!msgSendSuperArgs.empty()) {
                    msgSendSuperCall = ctx.factory.CreatePropSetterCallViaMsgSendSuper(
                        *targetFd->propDecl, std::move(receiver), ASTCloner::Clone(objCSuper.get()), std::move(msgSendSuperArgs[0]));
                } else {
                    msgSendSuperCall = ctx.factory.CreatePropGetterCallViaMsgSendSuper(
                        *targetFd->propDecl, std::move(receiver), ASTCloner::Clone(objCSuper.get()));
                }
            } else {
                msgSendSuperCall = ctx.factory.CreateMethodCallViaMsgSendSuper(
                    *targetFd, std::move(receiver), ASTCloner::Clone(objCSuper.get()), std::move(msgSendSuperArgs));
            }

            if (targetFd->HasAnno(AST::AnnotationKind::OBJ_C_OPTIONAL)) {
                auto methodSelector = ctx.nameGenerator.GetObjCDeclName(*targetFd);
                auto superClass = ctx.factory.CreateGetSuperClassExpr(std::move(objCSuper), curFile);
                auto guardCall = ctx.factory.CreateOptionalMethodGuard(std::move(msgSendSuperCall), std::move(superClass),
                    methodSelector, curFile);
                guardCall->curFile = curFile;
                return Nodes<Node>(std::move(guardCall));
            }

            return Nodes<Node>(std::move(msgSendSuperCall));
        });
    withMethodEnvCall->curFile = curFile;
    ce.desugarExpr = ctx.factory.WrapEntity(std::move(withMethodEnvCall), *targetFdTy->retTy);
}

void DesugarImpls::DesugarGetForPropDecl(
    InteropContext& ctx, ClassDecl& impl, MemberAccess& ma)
{
    if (ma.desugarExpr) {
        return;
    }

    auto target = ma.GetTarget();
    if (!target || target->astKind != ASTKind::PROP_DECL || target->TestAttr(Attribute::DESUGARED_MIRROR_FIELD)) {
        return;
    }

    auto isSuper = false;
    if (auto re = As<ASTKind::REF_EXPR>(ma.baseExpr); re) {
        isSuper = re->isSuper;
    }

    if (!isSuper) {
        return;
    }

    auto pd = StaticAs<ASTKind::PROP_DECL>(target);
    if (!ctx.typeMapper.IsObjCMirror(*pd->outerDecl->GetTy())) {
        return;
    }
    auto nativeHandle = ctx.factory.CreateNativeHandleExpr(impl, false, ma.curFile);
    auto withMethodEnvCall = ctx.factory.CreateWithMethodEnvScope(
        std::move(nativeHandle), impl, ma.GetTy(), [&](auto&& receiver, auto&& objCSuper) {
            auto msgSendSuperCall =
                ctx.factory.CreatePropGetterCallViaMsgSendSuper(*pd, std::move(receiver), std::move(objCSuper));

            return Nodes<Node>(std::move(msgSendSuperCall));
        });
    withMethodEnvCall->curFile = ma.curFile;
    ma.desugarExpr = ctx.factory.WrapEntity(std::move(withMethodEnvCall), *ma.GetTy());
}
} // namespace Cangjie::Interop::ObjC
