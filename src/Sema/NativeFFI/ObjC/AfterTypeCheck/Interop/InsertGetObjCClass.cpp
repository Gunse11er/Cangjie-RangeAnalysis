// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements inserting $getObjCClass function in Objective-C mirrors and impls
 */

#include "NativeFFI/ObjC/Utils/Common.h"
#include "Handlers.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;
using namespace Cangjie::Native::FFI;

void InsertGetObjCClass::HandleImpl(InteropContext& ctx)
{
    for (auto& mirror : ctx.mirrors) {
        if (mirror->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }

        if (mirror->astKind == ASTKind::INTERFACE_DECL) {
            auto interfaceMirror = As<ASTKind::INTERFACE_DECL>(mirror);
            auto getObjCClassDecl = ctx.factory.CreateGetObjCClassDecl(*interfaceMirror);
            CJC_NULLPTR_CHECK(getObjCClassDecl);
            getObjCClassDecl->funcBody->body =
                CreateBlock(Nodes(ctx.factory.CreateThrowStaticMethodCallOnInterfaceExpr(*mirror->curFile)),
                    getObjCClassDecl->GetTy());
            interfaceMirror->body->decls.emplace_back(std::move(getObjCClassDecl));
        } else if (mirror->astKind == ASTKind::CLASS_DECL) {
            auto classMirror = As<ASTKind::CLASS_DECL>(mirror);
            auto getObjCClassFunc = ctx.factory.CreateGetObjCClass(*classMirror);
            CJC_NULLPTR_CHECK(getObjCClassFunc);
            classMirror->body->decls.emplace_back(std::move(getObjCClassFunc));
        }
    }

    for (auto& impl : ctx.impls) {
        if (impl->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }

        auto getObjCClassFunc = ctx.factory.CreateGetObjCClass(*impl);
        CJC_NULLPTR_CHECK(getObjCClassFunc);
        impl->body->decls.emplace_back(std::move(getObjCClassFunc));
    }
}
