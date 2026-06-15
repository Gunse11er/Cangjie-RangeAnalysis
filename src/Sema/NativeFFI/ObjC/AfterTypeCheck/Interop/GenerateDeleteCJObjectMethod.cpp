// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements generating delete Cangjie object method for Objective-C mirror subtypes.
 */

#include "Handlers.h"
#include "NativeFFI/ObjC/Utils/Common.h"
#include "cangjie/AST/Match.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;

void GenerateDeleteCJObjectMethod::HandleImpl(InteropContext& ctx)
{
    auto genNativeDeleteMethod = [this, &ctx](Decl& decl) {
        if (decl.TestAttr(Attribute::IS_BROKEN)) {
            return;
        }
        bool forOneWayMapping = false;
        forOneWayMapping = this->interopType == InteropType::CJ_Mapping && ctx.typeMapper.IsOneWayMapping(decl);
        auto deleteCjObject = ctx.factory.CreateDeleteCjObject(decl, forOneWayMapping);
        CJC_ASSERT(deleteCjObject);
        ctx.genDecls.emplace_back(std::move(deleteCjObject));
    };

    auto genNativeDeleteMethodForGeneric = [this, &ctx](Decl& decl,
            const std::vector<Native::FFI::GenericConfigInfo*>& genericConfigsVector) {
        if (decl.TestAttr(Attribute::IS_BROKEN)) {
            return;
        }
        bool forOneWayMapping = false;
        forOneWayMapping = this->interopType == InteropType::CJ_Mapping && ctx.typeMapper.IsOneWayMapping(decl);
        for (auto genericConfig : genericConfigsVector) {
            auto deleteCjObject = ctx.factory.CreateDeleteCjObject(decl, forOneWayMapping, genericConfig);
            CJC_ASSERT(deleteCjObject);
            ctx.genDecls.emplace_back(std::move(deleteCjObject));
        }
    };

    if (interopType == InteropType::ObjC_Mirror) {
        for (auto& impl : ctx.impls) {
            // generate only for root @ObjCImpl classes
            if (HasImplSuperClass(*impl)) {
                continue;
            }
            genNativeDeleteMethod(*impl);
        }
    } else if (interopType == InteropType::CJ_Mapping) {
        for (auto& cjmapping : ctx.cjMappings) {
            std::vector<Native::FFI::GenericConfigInfo*> genericConfigsVector;
            bool isGenericGlueCode = false;
            Native::FFI::InitGenericConfigs(*cjmapping->curFile, cjmapping.get(), genericConfigsVector, isGenericGlueCode);
            if (isGenericGlueCode) {
                genNativeDeleteMethodForGeneric(*cjmapping, genericConfigsVector);
            } else {
                genNativeDeleteMethod(*cjmapping);
            }
        }
    }
}
