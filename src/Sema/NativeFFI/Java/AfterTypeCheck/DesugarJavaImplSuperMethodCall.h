// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares after-typecheck Java interop stage: desugaring super method calls for java impls.
 */
#ifndef CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_METHOD_CALL
#define CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_METHOD_CALL

#include "Context.h"
#include "InteropLibBridge.h"
#include "cangjie/AST/Node.h"

namespace Cangjie::Interop::Java {
class JavaDesugarManager;
}

namespace Cangjie::Native::FFI::Java {
using namespace Interop::Java;

class DesugarSuperMethodCallInJavaImplReferenceWrapper : public AfterTypeCheckStage {
public:
    explicit DesugarSuperMethodCallInJavaImplReferenceWrapper(JavaDesugarManager& man);
protected:
    void Process(AfterTypeCheckContext& ctx) override;
private:
    void DesugarSuperMethodCall(AST::CallExpr& call, AST::ClassDecl& impl) const;

    InteropLibBridge& ilib;
    Native::FFI::Java::Utils& utils;
};

}

#endif // CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_JAVA_IMPL_SUPER_METHOD_CALL
