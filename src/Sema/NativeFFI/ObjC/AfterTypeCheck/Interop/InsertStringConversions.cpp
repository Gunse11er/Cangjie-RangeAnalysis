// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements inserting string conversions class member bodies for specific mirror classes:
 *  - `init(str: String)` for NSString class;
 *  - `toString(): String` for NSObject class.
 */

#include "NativeFFI/ObjC/Utils/Common.h"
#include "Handlers.h"
#include "NativeFFI/Utils.h"
#include "cangjie/AST/AttributePack.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Utils/CheckUtils.h"
#include <string>
#include <string_view>

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;
using namespace Cangjie::Native::FFI;

void InsertStringConversions::HandleImpl(InteropContext& ctx)
{
    for (auto& mirror : ctx.mirrors) {
        if (mirror->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }

        if (mirror->astKind == ASTKind::CLASS_DECL) {
            auto classDecl = StaticCast<ClassDecl*>(mirror);

            auto foreignName = classDecl->identifier.Val();
            auto customName = Native::FFI::GetSingleArgumentAnnotationValue(*classDecl, AnnotationKind::OBJ_C_MIRROR);
            if (customName) {
                foreignName = customName;
            }

            if (foreignName == NSSTRING_CLASS_IDENT) {
                auto funcDecl = As<ASTKind::FUNC_DECL>(FindMirrorMember(*classDecl, IsObjCGeneratedNSStringCtor));
                CJC_NULLPTR_CHECK(funcDecl);
                CJC_ASSERT(funcDecl->funcBody->paramLists.size() > 0);
                auto& funcParams = funcDecl->funcBody->paramLists[0]->params;
                CJC_ASSERT(funcParams.size() == 1);
                auto funcParam = funcParams[0].get();
                auto funcParamRefExpr = CreateRefExpr(*funcParam);
                auto convertCall = ctx.factory.CreateConvertToNSStringCall(
                    std::move(funcParamRefExpr), *classDecl, classDecl->curFile);
                auto& block = funcDecl->funcBody->body;
                block->body.push_back(std::move(convertCall));
            }

            if (foreignName == NSOBJECT_CLASS_IDENT) {
                auto funcDecl = As<ASTKind::FUNC_DECL>(FindMirrorMember(*classDecl, IsObjCGeneratedNSObjectToString));
                CJC_NULLPTR_CHECK(funcDecl);
                auto nativeHandle = GetNativeVarHandle(*classDecl);
                auto nativeHandleRefExpr = CreateRefExpr(*nativeHandle);
                auto convertCall = ctx.factory.CreateDescriptionAsStringCall(std::move(nativeHandleRefExpr));
                auto& block = funcDecl->funcBody->body;
                block->body.push_back(std::move(convertCall));
            }
        }
    }
}
