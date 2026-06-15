// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares after-typecheck Java interop stage: desugaring of super constructor calls within java impls.
 */
#ifndef CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_CONSTRUCTOR_CALL
#define CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_CONSTRUCTOR_CALL

#include "Context.h"
#include "InteropLibBridge.h"
#include "cangjie/AST/Node.h"

namespace Cangjie::Interop::Java {
class JavaDesugarManager;
}

namespace Cangjie::Native::FFI::Java {
using namespace Interop::Java;

class DesugarJavaImplSuperConstructorCall : public AfterTypeCheckStage {
public:
    explicit DesugarJavaImplSuperConstructorCall(JavaDesugarManager& man);
protected:
    void Process(AfterTypeCheckContext& ctx) override;
private:
    void DesugarSuperConstructorCall(AfterTypeCheckContext& ctx, const AST::FuncDecl& ctor) const;

    /**
     * Generate native for arg:
     * ```cangjie
     * public @C func Java_xxx_superC${ctorId}A${argId}P${paramIds} (${usingParams}) {
     *     return withExceptionHandling(env, { =>
     *         let tmp = xxx.C${ctorId}A${argId}P${paramIds}(${usingParams})
     *         return unwrap(tmp)
     *     })
     * }
     * ```
     */
    OwnedPtr<AST::FuncDecl> CreateNativeFunc4Argument(AST::FuncDecl& memberFunc, AST::ClassLikeDecl& refWrapper) const;

    /**
     * Generates member proxy function for super-call argument computation within reference mapper.
     * With this function, argument computation happens within reference wrapper scope as user wrote,
     * requiring no extra AST modifications.
     * ```cangjie
     * internal func C${ctorId}A${argId}P${paramIds} (${usingParams}) {
     *     return ${clonedExpr($arg)}
     * }
     * ```
     */
    OwnedPtr<AST::FuncDecl> CreateMemberFunc4Argument(const AST::FuncArg& arg,
        const std::vector<OwnedPtr<AST::FuncParam>>& params, AST::ClassLikeDecl& refWrapper,
        size_t ctorId, size_t argId) const;

    /**
     * Unwrap a refExpr of native func param into Cangjie type.
     * ref: jobject -> @JavaMirror/@JavaImpl
     */
    OwnedPtr<AST::Expr> UnwrapRefExpr(OwnedPtr<AST::RefExpr> ref,
        Ptr<AST::Ty> targetTy, const AST::ClassLikeDecl& decl) const;

    /**
     * Wrap the cangjie expr into native type.
     * expr: @JavaMirror/@JavaImpl -> jobject
     */
    OwnedPtr<AST::Expr> WrapExprWithExceptionHandling(std::vector<OwnedPtr<AST::Node>>&& nodes,
        OwnedPtr<AST::Expr> expr, AST::FuncParam& env, const AST::ClassLikeDecl& decl) const;

    InteropLibBridge& ilib;
    TypeManager& typeManager;
    JavaDesugarManager& man;
};

}

#endif // CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_CONSTRUCTOR_CALL