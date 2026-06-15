// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares pre-typecheck Java interop stage: generation of reference wrapper classes for
 * @JavaMirror interfaces and abstract classes.
 */
#ifndef CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_REFERENCE_WRAPPER_CLASS
#define CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_REFERENCE_WRAPPER_CLASS

#include "JavaInteropManager.h"

namespace Cangjie::Native::FFI::Java {

/**
 * Generates wrapper class as a direct supertype for the following declarations:
 * - @JavaMirror abstract class
 * - @JavaMirror interface.
 * Generated class is used as a wrapper for java reference when actual subtype that came from java is not known.
 * Example of generated synthetic wrapper:
 * ```
 * // CL is interface or abstract class. If CL is interface then JObject is added as super class
 * class CL_impl <: CL {
 *     init(ref: Java_CFFI_JavaEntity) { // inherited from JObject
 *         $javaref = ref
 *     }
 *
 *     public func $getJavaRef() { // inherited from JObject
 *         return $javaref
 *     }
 * }
 * ```
 */
class GenerateJavaMirrorReferenceWrapperClass : public PreTypeCheckStage {
public:
    explicit GenerateJavaMirrorReferenceWrapperClass()
    {
    }
protected:
    void Process(PreTypeCheckContext& ctx) override;
private:
    OwnedPtr<AST::ClassDecl> GenerateWrapperClass(AST::ClassLikeDecl& mirror) const;
    
    /**
     * Returns true if declaration `cd` is a @JavaMirror that should have a class wrapper for java reference.
     */
    bool ShouldHaveMirrorReferenceWrapper(const AST::Node& node) const;
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_JAVA_MIRROR_REFERENCE_WRAPPER_CLASS
