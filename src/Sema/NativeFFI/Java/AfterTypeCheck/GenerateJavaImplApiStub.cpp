// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "GenerateJavaImplApiStub.h"
#include "NativeFFI/Utils.h"
#include "InteropLibBridge.h"

#include "cangjie/AST/Create.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Types.h"

namespace Cangjie::Native::FFI::Java {

OwnedPtr<AST::FuncDecl> GenerateJavaImplWrappingConstructorStub::CreateWrappingConstructorStub(
    ClassDecl& refWrapper) const
{
    auto ctor = CreateFuncDecl("init", CreateFuncBody({}, CreateRefType(refWrapper), CreateBlock({})));
    ctor->funcBody->paramLists.emplace_back(MakeOwned<FuncParamList>());
    ctor->funcBody->paramLists[0]->EnableAttr(Attribute::COMPILER_ADD);
    auto& params = ctor->funcBody->paramLists[0]->params;

    // obj: Java_CFFI_JavaEntity
    auto javaEntityDecl = ilib.GetJavaEntityDecl();
    if (!javaEntityDecl) {
        return nullptr;
    }

    params.emplace_back(CreateFuncParam(
        JAVA_IMPL_ENTITY_ARG_NAME_IN_GENERATED_CTOR, CreateRefType(*javaEntityDecl), nullptr, javaEntityDecl->GetTy()));
    auto& entityParam = *params[0];

    // regId: RegistryId (Int64)
    auto& regIdParam = man.PushSelfParams(params);

    std::vector<Ptr<Ty>> paramTys;
    paramTys.push_back(entityParam.GetTy());
    paramTys.push_back(regIdParam.GetTy());
    auto ctorTy = typeManager.GetFunctionTy(paramTys, refWrapper.GetTy());

    ctor->funcBody->SetTy(ctorTy);
    ctor->SetTy(ctorTy);
    ctor->funcBody->funcDecl = ctor.get();
    ctor->EnableAttr(Attribute::CONSTRUCTOR);

    auto& block = ctor->funcBody->body;
    block->SetTy(refWrapper.GetTy());

    return ctor;
}

FuncDecl& GenerateJavaImplWrappingConstructorStub::InsertWrappingConstructor(ClassDecl& refWrapper) const
{
    auto ctor = WithinFile(CreateWrappingConstructorStub(refWrapper), refWrapper.curFile);
    auto& res = *ctor;
    ctor->outerDecl = &refWrapper;
    ctor->fullPackageName = refWrapper.fullPackageName;
    ctor->moduleName = refWrapper.moduleName;
    ctor->begin = refWrapper.body->begin;
    ctor->end = refWrapper.body->begin;
    ctor->funcBody->parentClassLike = &refWrapper;
    ctor->EnableAttr(Attribute::IN_CLASSLIKE, Attribute::PUBLIC);
    refWrapper.GetMemberDecls().emplace_back(std::move(ctor));
    return res;
}

GenerateJavaImplWrappingConstructorStub::GenerateJavaImplWrappingConstructorStub(JavaDesugarManager& man)
    : man(man), typeManager(man.typeManager), ilib(man.lib)
{
}

void GenerateJavaImplWrappingConstructorStub::Process(AfterTypeCheckContext& ctx)
{
    for (auto refWrapper : ctx.GetJavaImplReferenceWrappers()) {
        auto& ctor = InsertWrappingConstructor(*refWrapper);
        ctx.CacheJavaImplWrappingConstructor(*refWrapper, ctor);
    }
}

}