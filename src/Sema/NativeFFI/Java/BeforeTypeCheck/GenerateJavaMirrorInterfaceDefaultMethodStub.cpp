// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GenerateJavaMirrorInterfaceDefaultMethodStub.h"
#include "JavaInteropManager.h"
#include "cangjie/AST/Match.h"
#include "Utils.h"
#include "cangjie/AST/Utils.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;
using namespace Cangjie::Native::FFI;
using namespace std;

namespace Cangjie::Native::FFI::Java {

namespace {
void RemoveAbstractAttributeForJavaHasDefaultMethods(const InterfaceDecl& decl)
{
    for (const auto& member : decl.GetMemberDeclPtrs()) {
        if (member->TestAttr(Attribute::JAVA_HAS_DEFAULT)) {
            member->DisableAttr(Attribute::ABSTRACT);
            /*
            cj and java have different typechecks,
            default attribute makes this difference.
            */
            member->EnableAttr(Attribute::DEFAULT);
        }
    }
}

} // namespace

void GenerateJavaMirrorInterfaceDefaultMethodStub::InsertJavaHasDefaultMethodStubs(PreTypeCheckContext& ctx,
    const InterfaceDecl& id)
{
    for (auto& decl : id.GetMemberDeclPtrs()) {
        if (auto fd = As<ASTKind::FUNC_DECL>(decl);
            fd && fd->TestAttr(Attribute::JAVA_HAS_DEFAULT)) {
            InsertMethodStub(*fd, ctx.importManager, ctx.typeManager);
        }
    }
}

void GenerateJavaMirrorInterfaceDefaultMethodStub::Process(PreTypeCheckContext& ctx)
{
    for (auto mirror : ctx.javaMirrors) {
        if (auto mirrorInterface = As<ASTKind::INTERFACE_DECL>(mirror)) {
            RemoveAbstractAttributeForJavaHasDefaultMethods(*mirrorInterface);
            InsertJavaHasDefaultMethodStubs(ctx, *mirrorInterface);
        }
    }
}

} // Cangjie::Native::FFI::Java
