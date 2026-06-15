// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements generating Objective-C glue code.
 */

#include "NativeFFI/ObjC/ObjCCodeGenerator/ObjCGenerator.h"
#include "Handlers.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::ObjC;

void GenerateGlueCode::HandleImpl(InteropContext& ctx)
{
    auto genGlueCode = [this, &ctx](Decl& decl) {
        if (decl.TestAnyAttr(Attribute::IS_BROKEN, Attribute::HAS_BROKEN)) {
            return;
        }
        auto codegen = ObjCGenerator(ctx, &decl, ctx.outputObjCGenDir, ctx.cjLibOutputPath, this->interopType);
        codegen.Generate();
    };

    // For Generic Glue Code
    auto genGlueCodeWithGenericConfigs = [this, &ctx](Decl& decl, Native::FFI::GenericConfigInfo* genericConfig,
        bool isGenericGlueCode) {
        if (decl.TestAnyAttr(Attribute::IS_BROKEN, Attribute::HAS_BROKEN)) {
            return;
        }
        auto codegen = ObjCGenerator(
            ctx,
            &decl,
            ctx.outputObjCGenDir,
            ctx.cjLibOutputPath,
            this->interopType,
            genericConfig,
            isGenericGlueCode
        );
        codegen.Generate();
    };

    auto processContainer = [&genGlueCode, &genGlueCodeWithGenericConfigs](auto& container) {
        for (auto& item : container) {
            std::vector<Native::FFI::GenericConfigInfo*> genericConfigsVector;
            bool isGenericGlueCode = false;
            Native::FFI::InitGenericConfigs(
                *item->curFile,
                item.get(),
                genericConfigsVector,
                isGenericGlueCode
            );
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    genGlueCodeWithGenericConfigs(*item, genericConfig, isGenericGlueCode);
                }
            } else {
                genGlueCode(*item);
            }
        }
    };

    switch (interopType) {
        case InteropType::ObjC_Mirror:
            for (auto& impl : ctx.impls) {
                genGlueCode(*impl);
            }
            break;
        case InteropType::CJ_Mapping:
            processContainer(ctx.cjMappings);
            break;
        case InteropType::CJ_Mapping_Interface:
            processContainer(ctx.cjMappingInterfaces);
            break;
        default:
            break;
    }
}
