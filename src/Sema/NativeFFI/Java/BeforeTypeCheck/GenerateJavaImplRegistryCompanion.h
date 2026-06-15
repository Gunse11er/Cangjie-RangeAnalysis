// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares pre-typecheck Java interop stage: generation of @JavaImpl registry companion class.
 * An instance of that class is stored in registry.
 * An actual state (fields with values) of java impl object is represented within registry companion.
 *
 */
#ifndef CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_IMPL_REGISTRY_COMPANION_CLASS
#define CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_IMPL_REGISTRY_COMPANION_CLASS

#include "JavaInteropManager.h"

namespace Cangjie::Native::FFI::Java {

/**
 * For every @JavaImpl class, it generates corresponding registry companion class.
 *
 * As an example, generation scheme looks like:
 * ```cangjie
 * private class Foo$reg {
 * }
 * ```
 *
 * Corresponding (user-written) impl class:
 * ```cangjie
 * @JavaImpl
 * public class Foo <: JObject {
 * }
 * ```
 */
class GenerateJavaImplRegistryCompanionClass : public PreTypeCheckStage {
public:
    explicit GenerateJavaImplRegistryCompanionClass()
    {
    }
protected:
    void Process(PreTypeCheckContext& ctx) override;
private:
    OwnedPtr<AST::ClassDecl> GenerateRegistryCompanion(AST::ClassDecl& impl) const;
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_IMPL_REGISTRY_COMPANION_CLASS
