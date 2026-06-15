// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements generating init Cangjie object method for @ObjCImpls and CJMapping.
 */

#include "Handlers.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;

void GenerateInitCJObjectMethods::GenNativeInitMethodForEnumCtor(InteropContext& ctx, AST::EnumDecl& enumDecl,
    bool isGenericGlueCode, const std::vector<Native::FFI::GenericConfigInfo*>& genericConfigsVector)
{
    if (enumDecl.TestAttr(Attribute::IS_BROKEN)) {
        return;
    }

    if (this->interopType == InteropType::ObjC_Mirror) {
        return;
    }

    for (auto& ctor : enumDecl.constructors) {
        OwnedPtr<Decl> initCjObject;
        if (ctor->astKind == ASTKind::FUNC_DECL) {
            auto fd = As<ASTKind::FUNC_DECL>(ctor.get());
            CJC_NULLPTR_CHECK(fd);
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    initCjObject = ctx.factory.CreateInitCjObject(enumDecl, *fd, true, genericConfig);
                    CJC_ASSERT(initCjObject);
                    ctx.genDecls.emplace_back(std::move(initCjObject));
                }
            } else {
                initCjObject = ctx.factory.CreateInitCjObject(enumDecl, *fd, true);
                CJC_ASSERT(initCjObject);
                ctx.genDecls.emplace_back(std::move(initCjObject));
            }
        } else if (ctor->astKind == ASTKind::VAR_DECL) {
            auto varDecl = As<ASTKind::VAR_DECL>(ctor.get());
            initCjObject = ctx.factory.CreateInitCjObjectForEnumNoParams(enumDecl, *varDecl);
            CJC_ASSERT(initCjObject);
            ctx.genDecls.emplace_back(std::move(initCjObject));
        }
    }
}

void GenerateInitCJObjectMethods::HandleImpl(InteropContext& ctx)
{
    auto genNativeInitMethod = [this, &ctx](Decl& decl, bool isGenericGlueCode,
            const std::vector<Native::FFI::GenericConfigInfo*>& genericConfigsVector) {
        if (decl.TestAttr(Attribute::IS_BROKEN)) {
            return;
        }
        for (auto& memberDecl : decl.GetMemberDeclPtrs()) {
            if (memberDecl->TestAttr(Attribute::IS_BROKEN)) {
                continue;
            }

            if (!memberDecl->TestAttr(Attribute::CONSTRUCTOR)) {
                continue;
            }

            if (!memberDecl->TestAttr(Attribute::PUBLIC)) {
                continue;
            }

            if (memberDecl->astKind != ASTKind::FUNC_DECL) {
                // skip primary ctor, as it is desugared to init already
                continue;
            }

            CJC_ASSERT_WITH_MSG(memberDecl->astKind == ASTKind::FUNC_DECL,
                "Expected ASTKind::FUNC_DECL, found " + ASTKIND_TO_STR.at(memberDecl->astKind));

            auto& ctorDecl = *StaticAs<ASTKind::FUNC_DECL>(memberDecl);

            // skip original ctors
            if (interopType == InteropType::ObjC_Mirror && !ctx.factory.IsGeneratedCtor(ctorDecl)) {
                continue;
            }
            if (interopType == InteropType::CJ_Mapping && !ctx.typeMapper.IsObjCCJMappingMember(ctorDecl)) {
                continue;
            }

            OwnedPtr<FuncDecl> initCjObject;
            if (interopType == InteropType::CJ_Mapping) {
                bool forOneWayMapping = false;
                forOneWayMapping = interopType == InteropType::CJ_Mapping && ctx.typeMapper.IsOneWayMapping(decl);
                if (isGenericGlueCode) {
                    for (auto genericConfig : genericConfigsVector) {
                        initCjObject = ctx.factory.CreateInitCjObject(decl, ctorDecl, forOneWayMapping, genericConfig);
                        CJC_ASSERT(initCjObject);
                        ctx.genDecls.emplace_back(std::move(initCjObject));
                    }
                } else {
                    initCjObject = ctx.factory.CreateInitCjObject(decl, ctorDecl, forOneWayMapping);
                    CJC_ASSERT(initCjObject);
                    ctx.genDecls.emplace_back(std::move(initCjObject));
                }
            } else if (interopType == InteropType::ObjC_Mirror) {
                initCjObject = ctx.factory.CreateInitCjObjectReturningObjCSelf(decl, ctorDecl);
                CJC_ASSERT(initCjObject);
                ctx.genDecls.emplace_back(std::move(initCjObject));
            }
        }
    };

    if (interopType == InteropType::ObjC_Mirror) {
        for (auto& impl : ctx.impls) {
            genNativeInitMethod(*impl, false, {});
        }
    } else if (interopType == InteropType::CJ_Mapping) {
        for (auto& cjmapping : ctx.cjMappings) {
            std::vector<Native::FFI::GenericConfigInfo*> genericConfigsVector;
            bool isGenericGlueCode = false;
            Native::FFI::InitGenericConfigs(*cjmapping->curFile, cjmapping.get(), genericConfigsVector, isGenericGlueCode);
            if (auto enumDecl = As<ASTKind::ENUM_DECL>(cjmapping)) {
                GenNativeInitMethodForEnumCtor(ctx, *enumDecl, isGenericGlueCode, genericConfigsVector);
            } else {
                genNativeInitMethod(*cjmapping, isGenericGlueCode, genericConfigsVector);
            }
        }
    }

}
