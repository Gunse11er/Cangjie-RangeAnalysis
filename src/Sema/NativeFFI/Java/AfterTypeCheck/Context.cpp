// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Context.h"
#include "cangjie/AST/AttributePack.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "NativeFFI/Java/Utils.h"

namespace Cangjie::Native::FFI::Java {
using namespace AST;

std::vector<Ptr<ClassLikeDecl>> AfterTypeCheckContext::GetJavaMirrors() const
{
    return javaMirrors;
}

std::vector<Ptr<ClassDecl>> AfterTypeCheckContext::GetJavaImplReferenceWrappers() const
{
    return javaImplReferenceWrappers;
}

std::vector<Ptr<ClassDecl>> AfterTypeCheckContext::GetJavaImplRegistryCompanions() const
{
    return javaImplRegistryCompanions;
}

ClassDecl& AfterTypeCheckContext::GetJavaImplRegistryCompanion(const ClassDecl& referenceWrapper) const
{
    Ptr<ClassDecl> companion;
    for (auto registryCompanion : GetJavaImplRegistryCompanions()) {
        if (registryCompanion->identifier == GetImplRegistryCompanionClassName(referenceWrapper)) {
            companion = registryCompanion;
            break;
        }
    }

    CJC_NULLPTR_CHECK(companion);
    return *companion;
}

void AfterTypeCheckContext::CacheJavaImplReferenceWrapperConstructorsPair(FuncDecl& userDefinedCtor,
    FuncDecl& generatedCtor)
{
    javaImplRefWrapperCjToJavaConstructors[&userDefinedCtor] = &generatedCtor;
}

FuncDecl& AfterTypeCheckContext::GetJavaImplReferenceWrapperGeneratedConstructor(FuncDecl& userDefinedCtor)
{
    auto javaSideCtor = javaImplRefWrapperCjToJavaConstructors[&userDefinedCtor];
    CJC_NULLPTR_CHECK(javaSideCtor);
    return *javaSideCtor;
}

void AfterTypeCheckContext::CacheJavaImplRegistryCompanionReferenceField(ClassDecl& refWrapper, VarDecl& field)
{
    javaImplRegistryCompanionRefFields[&refWrapper] = &field;
}

VarDecl& AfterTypeCheckContext::GetJavaImplRegistryCompanionReferenceField(ClassDecl& refWrapper)
{
    auto registryCompanionRefField = javaImplRegistryCompanionRefFields[&refWrapper];
    CJC_NULLPTR_CHECK(registryCompanionRefField);
    return *registryCompanionRefField;
}

void AfterTypeCheckContext::CacheJavaImplWrappingConstructor(ClassDecl& refWrapper, FuncDecl& wrappingCtor)
{
    javaImplWrappingConstructors[&refWrapper] = &wrappingCtor;
}

FuncDecl& AfterTypeCheckContext::GetJavaImplWrappingConstructor(ClassDecl& refWrapper)
{
    auto wrappingCtor = javaImplWrappingConstructors[&refWrapper];
    CJC_NULLPTR_CHECK(wrappingCtor);
    return *wrappingCtor;
}

std::vector<Ptr<FuncDecl>> AfterTypeCheckContext::GetJavaImplUserDefinedConstructors(ClassDecl& refWrapper)
{
    std::vector<Ptr<FuncDecl>> ctors;

    for (auto member : refWrapper.GetMemberDeclPtrs()) {
        if (!member->TestAttr(Attribute::CONSTRUCTOR)) {
            continue;
        }

        // Skip original primary constructors and only consider its FuncDecl counterpart.
        auto ctor = As<ASTKind::FUNC_DECL>(member);
        if (!ctor) {
            continue;
        }
        auto hasParams = !ctor->funcBody->paramLists.empty() && !ctor->funcBody->paramLists[0]->params.empty();

        // Primary constructor is desugared as fresh FuncDecl with COMPILER_ADD attribute.
        auto isPrimary = member->TestAttr(Attribute::PRIMARY_CONSTRUCTOR, Attribute::COMPILER_ADD);
        // Visit implicit constructors too (compiler added without any parameters).
        auto isImplicit = member->TestAttr(Attribute::COMPILER_ADD, Attribute::IMPLICIT_ADD) && !hasParams;
        if (!isPrimary && !isImplicit && member->TestAttr(Attribute::COMPILER_ADD)) {
            continue;
        }

        ctors.emplace_back(ctor);
    }
    return ctors;
}

void AfterTypeCheckContext::AddGeneratedDecl(OwnedPtr<Decl>&& decl)
{
    CJC_ASSERT(decl->outerDecl == nullptr);
    generated.emplace_back(std::move(decl));
}

void AfterTypeCheckContext::FlushGeneratedDecls()
{
    for (auto& generatedDecl : generated) {
        /*
         * Make the generated declaration visible and available via ImportManager/CjoManager.
         */
        importManager.GetCjoManager()->AddGeneratedDeclToDeclMap(*generatedDecl);
        auto& fileDecls = generatedDecl->curFile->decls;
        fileDecls.push_back(std::move(generatedDecl));
    }
    generated.clear();
}

void AfterTypeCheckStage::operator()(AfterTypeCheckContext& ctx)
{
    Process(ctx);
    ctx.FlushGeneratedDecls();
}

AfterTypeCheckContext::AfterTypeCheckContext(const ImportManager& importManager, TypeManager& typeManager,
    AST::Package& pkg) : importManager(importManager), typeManager(typeManager), pkg(pkg),
    javaMirrors(Native::FFI::Java::GetJavaMirrors(pkg)),
    javaImplReferenceWrappers(Native::FFI::Java::GetJavaImpls(pkg)),
    javaImplRegistryCompanions(Native::FFI::Java::GetJavaImplRegistryCompanions(pkg))
{
}

} // namespace Cangjie::Interop::Java
