// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares after-typecheck Java interop stage: generating members within @JavaImpl reference wrappers.
 */
#ifndef CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_NATIVE_BRIDGE_FOR_JAVA_IMPL
#define CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_NATIVE_BRIDGE_FOR_JAVA_IMPL

#include "Context.h"
#include "cangjie/AST/Node.h"

namespace Cangjie::Interop::Java {
class JavaDesugarManager;
}

namespace Cangjie::Native::FFI::Java {
using namespace Interop::Java;

/**
 * Generates native @C functions as a bridge for @JavaImpl types to be callable from Java.
 */
class GenerateNativeBridgeForJavaImpl : public AfterTypeCheckStage {
public:
    explicit GenerateNativeBridgeForJavaImpl(JavaDesugarManager& man) : man(man)
    {
    }
protected:
    void Process(AfterTypeCheckContext& ctx) override;
private:
    void Process(AST::ClassDecl& companion, AST::ClassDecl& refWrapper, AfterTypeCheckContext& ctx) const;

    /**
     * <id> - identifier assembled as "Java_<java side JNI identifier>_initCJObject_<mangle>".
     * Identifier prefix should follow JNI naming convention for native functions.
     * Mangled suffix is added to distinguish between overloaded constructors.
     *
     * ```
     * @C public func <id>(env: JNIEnv_ptr, obj: jobject, jniParams...): Unit {
     *      withExceptionHandling(env, { =>
     *          <reference wrapper constructor>($obj, <registry companion constructor>($obj), Wrap(jniParams...)...)
     *      })
     * }
     * ```
     */
    OwnedPtr<AST::FuncDecl> CreateConstructorBridge(
        AST::FuncDecl& refWrapperSampleCtor, AST::FuncDecl& refWrapperGeneratedCtor,
        AST::FuncDecl& registryCompanionCtor, AST::ClassDecl& companion) const;

    /**
     * For method accessible in java, the bridging function must be generated.
     */
    OwnedPtr<AST::FuncDecl> CreateMethodBridge(AfterTypeCheckContext& ctx,
        AST::FuncDecl& refWrapperFunc, AST::ClassDecl& companion) const;

    OwnedPtr<AST::FuncDecl> CreateStaticMethodBridge(AST::FuncDecl& refWrapperFunc, AST::ClassDecl& companion) const;

    /**
     * For method [foo] of class package.I:
     * ```cangbjie
     * func foo(a1: A, a2: B, ..., an: N): Ret {
     *     ...body...
     * }
     * ```
     *
     * the following bridging native function will be generated:
     * ```cangjie
     * @C
     * func Java_package_I_fooImpl{mangling}(env: JNIEnv_ptr, _: jobject, regId: jlong, a1: A, a2: B, ..., an: N): Ret {
     *     *WrapJavaEntity*(I(env, regId).foo(a1, a2, ..., an))
     * }
     * ```
     * Note: wrapping constructor of reference wrapper is used for java impl object instantiation.
     */
    OwnedPtr<AST::FuncDecl> CreateInstanceMethodBridge(AfterTypeCheckContext& ctx,
        AST::FuncDecl& refWrapperFunc, AST::ClassDecl& companion) const;

    /**
     * ```cangjie
     * @C public func Java_<type_signature>_deleteCJObject(env: JNIEnv_ptr, obj: jobject, regId: jlong): Unit {
     *      withExceptionHandling(env) {
     *          Java_CFFI_removeFromRegistry(regId)
     *     }
     * }
     * ```
     */
    OwnedPtr<AST::Decl> CreateFinalizationBridge(AST::ClassDecl& refWrapper) const;

    /**
     * Creates @C function with name `name`, return type `retTy` within `curFile` at `fullPackageName` at `moduleName`.
     * Appends `userParams` to native function parameters to comply with JNI ABI.
     * Parameters order of @C function: [jniEnv, objOrClass, <userParams>].
     */
    OwnedPtr<AST::FuncDecl> CreateNativeJavaABIFunc(std::string& name,
        std::vector<OwnedPtr<AST::FuncParam>> userParams, Ptr<AST::Ty> retTy,
        AST::File& curFile, std::string& moduleName, std::string& fullPackageName,
        std::function<void(
            AST::FuncDecl& f,
            AST::FuncParam& jniEnv,
            AST::FuncParam& objOrClass,
            std::vector<Ptr<AST::FuncParam>> userParams)> builder) const;

    /**
     * Creates new CType parameter based on java-compatible parameter `sample`.
     * Resulting type is consistent with java native method types ABI.
     */
    OwnedPtr<AST::FuncParam> ConvertJavaCompatibleToCTypeParam(AST::FuncParam& sample, AST::File& curFile) const;

    /**
     * Creates new CType parameters based on java-compatible parameters `sample`.
     * Resulting types are consistent with java native method types ABI.
     */
    std::vector<OwnedPtr<AST::FuncParam>> ConvertJavaCompatibleToCTypeParams(
        std::vector<OwnedPtr<AST::FuncParam>>& sample, AST::File& curFile) const;

    /**
     * Transforms CType expression to java-compatible expression.
     */
    OwnedPtr<AST::Expr> UnwrapCTypeExprAsJavaCompatible(OwnedPtr<AST::Expr> cexpr, Ptr<AST::Ty> javaCompatibleTy,
        Ptr<AST::Decl> outerDecl) const;

    JavaDesugarManager& man;
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_GENERATE_NATIVE_BRIDGE_FOR_JAVA_IMPL
