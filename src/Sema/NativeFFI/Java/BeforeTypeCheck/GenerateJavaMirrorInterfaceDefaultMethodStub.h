// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares pre-typecheck Java interop stage: rewritting of fields to the properties with stubs
 * within @JavaMirror classes.
 *
 * Generated property stubs are replaced with corresponding java calls at later stages.
 *
 */
#ifndef CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_INTERFACE_DEFAULT_METHOD_STUB
#define CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_INTERFACE_DEFAULT_METHOD_STUB

#include "JavaInteropManager.h"

namespace Cangjie::Native::FFI::Java {

/**
 * Generates method body stub for @JavaMirror methods declared as methods with default implementation at java.
 */
class GenerateJavaMirrorInterfaceDefaultMethodStub : public PreTypeCheckStage {
public:
    explicit GenerateJavaMirrorInterfaceDefaultMethodStub()
    {
    }
protected:
    void Process(PreTypeCheckContext& ctx) override;
private:
    void InsertJavaHasDefaultMethodStubs(PreTypeCheckContext& ctx, const AST::InterfaceDecl& id);
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_INTERFACE_DEFAULT_METHOD_STUB
