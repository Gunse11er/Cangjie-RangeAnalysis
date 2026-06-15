// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements generating and inserting a forward class for each CJ-Mapping interface
 */

#include "Handlers.h"
#include "NativeFFI/ObjC/Utils/Common.h"
#include "NativeFFI/Utils.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;
using namespace Cangjie::Native::FFI;

OwnedPtr<ClassDecl> InsertFwdClasses::InitInterfaceFwdClassDecl(const Ptr<AST::ClassLikeDecl>& interfaceDecl)
{
    auto fwdclassDecl = MakeOwned<ClassDecl>();
    fwdclassDecl->identifier = interfaceDecl->identifier.Val() + OBJ_C_FWD_CLASS_SUFFIX;
    fwdclassDecl->identifier.SetPos(interfaceDecl->identifier.Begin(), interfaceDecl->identifier.End());
    fwdclassDecl->fullPackageName = interfaceDecl->fullPackageName;
    fwdclassDecl->moduleName = ::Cangjie::Utils::GetRootPackageName(interfaceDecl->fullPackageName);
    fwdclassDecl->curFile = interfaceDecl->curFile;

    fwdclassDecl->EnableAttr(Attribute::PUBLIC, Attribute::COMPILER_ADD, Attribute::CJ_MIRROR_OBJC_INTERFACE_FWD,  Attribute::ABSTRACT);

    fwdclassDecl->body = MakeOwned<ClassBody>();
    return fwdclassDecl;
}

OwnedPtr<FuncDecl> InsertFwdClasses::GenerateInterfaceFwdclassMethod(InteropContext& ctx,
    AST::ClassDecl& fwdclassDecl, FuncDecl& interfaceFuncDecl, Native::FFI::GenericConfigInfo* genericConfig)
{
    auto funcDecl = ASTCloner::Clone(Ptr<FuncDecl>(&interfaceFuncDecl));
    funcDecl->DisableAttr(Attribute::ABSTRACT, Attribute::OPEN);
    funcDecl->EnableAttr(Attribute::PUBLIC, Attribute::CJ_MIRROR_JAVA_INTERFACE_FWD);

    if (genericConfig) {
        ReplaceGenericTyForFunc(funcDecl, genericConfig, ctx.typeManager);
        funcDecl->funcBody->parentClassLike = Ptr<AST::ClassLikeDecl>(&fwdclassDecl);
    }
    funcDecl->outerDecl = Ptr<Decl>(&fwdclassDecl);

    return funcDecl;
}

void InsertFwdClasses::GenerateInterfaceFwdClassBody(InteropContext& ctx, AST::ClassDecl& fwdclassDecl, AST::ClassLikeDecl& interfaceDecl,
    Native::FFI::GenericConfigInfo* genericConfig)
{
    for (auto& decl : interfaceDecl.GetMemberDecls()) {
        if (FuncDecl* fd = As<ASTKind::FUNC_DECL>(decl.get());
            fd && !fd->TestAttr(Attribute::CONSTRUCTOR) && !fd->TestAttr(Attribute::STATIC)) {
            fwdclassDecl.body->decls.emplace_back(GenerateInterfaceFwdclassMethod(ctx, fwdclassDecl, *fd, genericConfig));
        }
    }
}

OwnedPtr<ClassDecl> InsertFwdClasses::GenerateGenericInterfaceFwdclassMethod(InteropContext& ctx,
    Ptr<AST::ClassLikeDecl>& interfaceDecl, Native::FFI::GenericConfigInfo* genericConfig)
{
    auto fwdclassDecl = InitInterfaceFwdClassDecl(interfaceDecl);
    fwdclassDecl->identifier = genericConfig->declInstName + OBJ_C_FWD_CLASS_SUFFIX;

    std::unordered_map<std::string, Ptr<Ty>> actualTyArgMap;
    std::vector<Ptr<Ty>> typeArgs;
    for (const auto& typePair : genericConfig->instTypes) {
        std::string typeStr = typePair.second;
        auto ty = GetTyByName(typeStr);
        typeArgs.push_back(ty);
        actualTyArgMap[typeStr] = ty;
    }
    auto instantTy = GetInstantyForGenericTy(*interfaceDecl, actualTyArgMap, ctx.typeManager);
    instantTy->typeArgs = typeArgs;
    // Set fwdclassDecl inheritedTypes.
    auto interfaceRefType = CreateRefType(*interfaceDecl, instantTy);
    std::vector<OwnedPtr<Type>> typeArguments;
    for (const auto& arg : actualTyArgMap) {
        auto priType = GetTypeByName(arg.first);
        interfaceRefType->typeArguments.emplace_back(std::move(priType));
    }
    fwdclassDecl->inheritedTypes.emplace_back(std::move(interfaceRefType));
    fwdclassDecl->SetTy(ctx.typeManager.GetClassTy(*fwdclassDecl, {}));

    auto classLikeTy = DynamicCast<ClassLikeTy*>(instantTy);
    CJC_ASSERT(classLikeTy);
    classLikeTy->directSubtypes.insert(fwdclassDecl->GetTy());
    GenerateInterfaceFwdClassBody(ctx, *fwdclassDecl, *interfaceDecl, genericConfig);
    return fwdclassDecl;
}

void InsertFwdClasses::HandleImpl(InteropContext& ctx)
{
    for (auto& interfaceDecl : ctx.cjMappingInterfaces) {
        std::vector<Native::FFI::GenericConfigInfo*> genericConfigsVector;
        bool isGenericGlueCode = false;
        Native::FFI::InitGenericConfigs(*interfaceDecl->curFile, interfaceDecl.get(), genericConfigsVector, isGenericGlueCode);
        if (isGenericGlueCode) {
            for (auto genericConfig : genericConfigsVector) {
                ctx.genDecls.emplace_back(GenerateGenericInterfaceFwdclassMethod(ctx, interfaceDecl, genericConfig));
            }
        } else {
            auto fwdclassDecl = InitInterfaceFwdClassDecl(interfaceDecl);
            fwdclassDecl->inheritedTypes.emplace_back(CreateRefType(*interfaceDecl));
            fwdclassDecl->SetTy(ctx.typeManager.GetClassTy(*fwdclassDecl, interfaceDecl->GetTy()->typeArgs));
            auto classLikeTy = DynamicCast<ClassLikeTy*>(interfaceDecl->GetTy());
            CJC_ASSERT(classLikeTy);
            classLikeTy->directSubtypes.insert(fwdclassDecl->GetTy());
            GenerateInterfaceFwdClassBody(ctx, *fwdclassDecl, *interfaceDecl);
            ctx.genDecls.emplace_back(std::move(fwdclassDecl));
        }
    }
}