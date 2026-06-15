// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares auxiliary methods for java interop implementation
 */
#include "Utils.h"
#include "NativeFFI/Java/AfterTypeCheck/Utils.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::Native::FFI::Java {
using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;

const Ptr<ClassDecl> GetSyntheticClass(const ImportManager& importManager, const ClassLikeDecl& cld)
{
    ClassDecl* synthetic =
        importManager.GetImportedDecl<ClassDecl>(cld.fullPackageName, GetMirrorReferenceWrapperNameFromClassLike(cld));

    CJC_NULLPTR_CHECK(synthetic);

    return Ptr(synthetic);
}

std::string GetMirrorReferenceWrapperNameFromClassLike(const ClassLikeDecl& mirrorDecl)
{
    constexpr auto wrapperNameSuffix = "$impl";
    CJC_ASSERT(mirrorDecl.IsInterfaceDecl() || mirrorDecl.IsAbstractClass());
    CJC_ASSERT(IsMirror(mirrorDecl));
    return mirrorDecl.identifier.Val() + wrapperNameSuffix;
}

std::string GetImplRegistryCompanionClassName(const ClassLikeDecl& javaImplDecl)
{
    constexpr auto wrapperNameSuffix = "$reg";
    CJC_ASSERT(IsImpl(javaImplDecl));
    return javaImplDecl.identifier.Val() + wrapperNameSuffix;
}

std::vector<Ptr<AST::ClassLikeDecl>> GetJavaMirrors(AST::File& file)
{
    std::vector<Ptr<ClassLikeDecl>> mirrors;
    for (auto& decl : file.decls) {
        if (IsMirror(*decl)) {
            auto mirror = StaticAs<ASTKind::CLASS_LIKE_DECL>(decl.get());
            mirrors.emplace_back(mirror);
        }
    }
    return mirrors;
}

std::vector<Ptr<AST::ClassLikeDecl>> GetJavaMirrors(AST::Package& pkg)
{
    std::vector<Ptr<ClassLikeDecl>> mirrors;
    for (auto& file : pkg.files) {
        auto fileMirrors = GetJavaMirrors(*file);
        std::move(fileMirrors.begin(), fileMirrors.end(), std::back_inserter(mirrors));
    }
    return mirrors;
}

std::vector<Ptr<AST::ClassDecl>> GetJavaImpls(AST::File& file)
{
    std::vector<Ptr<ClassDecl>> impls;
    for (auto& decl : file.decls) {
        if (IsImpl(*decl)) {
            if (auto impl = As<ASTKind::CLASS_DECL>(decl.get())) {
                impls.push_back(impl);
            }
        }
    }
    return impls;
}

std::vector<Ptr<AST::ClassDecl>> GetJavaImpls(AST::Package& pkg)
{
    std::vector<Ptr<ClassDecl>> impls;
    for (auto& file : pkg.files) {
        auto fileImpls = GetJavaImpls(*file);
        std::move(fileImpls.begin(), fileImpls.end(), std::back_inserter(impls));
    }
    return impls;
}

std::vector<Ptr<AST::ClassDecl>> GetJavaImplRegistryCompanions(AST::File& file)
{
    std::vector<Ptr<ClassDecl>> companions;
    for (auto& decl : file.decls) {
        if (IsImplRegistryCompanion(*decl)) {
            auto companion = StaticAs<ASTKind::CLASS_DECL>(decl.get());
            companions.emplace_back(companion);
        }
    }
    return companions;
}

std::vector<Ptr<AST::ClassDecl>> GetJavaImplRegistryCompanions(AST::Package& pkg)
{
    std::vector<Ptr<ClassDecl>> impls;
    for (auto& file : pkg.files) {
        auto fileImpls = GetJavaImplRegistryCompanions(*file);
        std::move(fileImpls.begin(), fileImpls.end(), std::back_inserter(impls));
    }
    return impls;
}

constexpr std::string_view JAVA_IMPL_REGISTRY_COMPANION_REFERENCE_FIELD_NAME_IN_REFERENCE_WRAPPER = "$reg";

bool IsJavaImplRegistryCompanionReferenceField(const AST::Node& node)
{
    if (node.astKind != ASTKind::VAR_DECL) {
        return false;
    }
    auto field = dynamic_cast<const VarDecl*>(&node);
    if (!field) {
        return false;
    }

    if (!field->outerDecl || !IsImplReferenceWrapper(*field->outerDecl)) {
        return false;
    }

    return field->identifier == JAVA_IMPL_REGISTRY_COMPANION_REFERENCE_FIELD_NAME_IN_REFERENCE_WRAPPER;
}

} // namespace Cangjie::Native::FFI::Java
