// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares after-typecheck Java interop stage: rewritting fields & access to them to the properties
 * within @JavaImpl reference wrappers.
 */
#ifndef CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_IMPL_REFERENCE_WRAPPER_FIELDS
#define CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_IMPL_REFERENCE_WRAPPER_FIELDS

#include "Context.h"
#include "JavaDesugarManager.h"
#include "cangjie/AST/Node.h"
#include <unordered_map>

namespace Cangjie::Native::FFI::Java {
using namespace Interop::Java;

/**
 * Rewrites fields to properties within @JavaImpl reference wrappers.
 * Replaces all reference wrapper field usages to the generated properties accessors.
 */
class RewriteJavaImplReferenceWrapperFields : public AfterTypeCheckStage {
public:
    explicit RewriteJavaImplReferenceWrapperFields(JavaDesugarManager& man,
        std::function<void(AST::Node&)> desugarPropRef) : man(man), desugarPropRef(desugarPropRef)
    {
    }
protected:
    void Process(AfterTypeCheckContext& ctx) override;
private:
    void CloneFields(AfterTypeCheckContext& ctx, AST::ClassDecl& refWrapper, AST::ClassDecl& registryCompanion) const;

    void RewriteFieldAccess(AfterTypeCheckContext& ctx, AST::Package& pkg) const;

    void EraseUserFields() const;

    OwnedPtr<AST::PropDecl> GenerateProxyProperty(AST::VarDecl& sample,
        AST::VarDecl& actualCompanionField, VarDecl& actualField) const;

    AST::VarDecl& CloneField(AST::VarDecl& sample, AST::ClassDecl& registryCompanion) const;

    /**
     * Cache for cloned field mapping.
     * Key: user-declared field (within reference wrapper).
     * Value: compiler-generated clone of the user field (within registry companion).
     */
    mutable std::unordered_map<Ptr<AST::VarDecl>, Ptr<AST::VarDecl>> clonedFields;
    /** Cache for generated property mapping.
     * Key: user-declared field (within reference wrapper).
     * Value: compiler-generated proxy property (within reference wrapper too).
     */
    mutable std::unordered_map<Ptr<AST::VarDecl>, OwnedPtr<AST::PropDecl>> generatedProps;

    JavaDesugarManager& man;
    /**
     * Due to re-resolve, early-stage property desugaring should happen again.
     */
    std::function<void(AST::Node&)> desugarPropRef;
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_AFTER_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_IMPL_REFERENCE_WRAPPER_FIELDS
