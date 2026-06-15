// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "JavaInteropManager.h"
#include "GenerateJavaImplApiStub.h"
#include "GenerateInJavaImplRegistryCompanion.h"
#include "DesugarJavaImplSuperConstructorCall.h"
#include "DesugarJavaImplSuperMethodCall.h"
#include "GenerateInJavaImplReferenceWrapper.h"
#include "RewriteJavaImplReferenceWrapperFields.h"
#include "GenerateNativeBridgeForJavaImpl.h"
#include "DesugarTypeCheckingAndCasting.h"
#include "DesugarJArray.h"
#include "NativeFFI/Java/AfterTypeCheck/InteropLibBridge.h"
#include <unordered_map>

namespace Cangjie::Interop::Java {

void JavaDesugarManager::ProcessJavaMirrorImplStage(AfterTypeCheckContext& ctx,
    std::function<void(AST::Node&)> desugarPropRef)
{
    for (auto& file : ctx.pkg.files) {
        GenerateInMirrors(*file, true);
    }

    Process<GenerateJavaImplWrappingConstructorStub>(ctx, *this);

    for (auto& file : ctx.pkg.files) {
        GenerateInMirrors(*file, false);
    }

    Process<GenerateInJavaImplRegistryCompanion>(ctx, *this);
    Process<DesugarJavaImplSuperConstructorCall>(ctx, *this);
    Process<GenerateInJavaImplReferenceWrapper>(ctx, *this);
    Process<DesugarSuperMethodCallInJavaImplReferenceWrapper>(ctx, *this);
    Process<RewriteJavaImplReferenceWrapperFields>(ctx, *this, desugarPropRef);
    Process<GenerateNativeBridgeForJavaImpl>(ctx, *this);

    for (auto& file : ctx.pkg.files) {
        DesugarMirrors(*file);
    }

    Process<DesugarJArray>(ctx, *this);
    GenerateJavaSourceCode(ctx);
    Process<DesugarTypeCheckingAndCasting>(ctx, *this);
}

void JavaDesugarManager::ProcessCJImplStage(DesugarCJImplStage stage, File& file)
{
    switch (stage) {
        case DesugarCJImplStage::PRE_GENERATE:
            PreGenerateInCJMapping(file);
            break;
        case DesugarCJImplStage::FWD_GENERATE:
            GenerateFwdClassInCJMapping(file);
            break;
        case DesugarCJImplStage::IMPL_GENERATE:
            GenerateInCJMapping(file);
            break;
        case DesugarCJImplStage::IMPL_DESUGAR:
            DesugarInCJMapping(file);
            break;
        default:
            CJC_ABORT(); // unreachable state
    }

    std::move(generatedDecls.begin(), generatedDecls.end(), std::back_inserter(file.decls));
    generatedDecls.clear();
}

void JavaInteropManager::DesugarPackage(Package& pkg,
    const std::unordered_map<Ptr<const InheritableDecl>,
    MemberMap>& memberMap,
    std::function<void(AST::Node&)> desugarPropRef)
{
    if (!(hasMirrorOrImpl || targetInteropLanguage == GlobalOptions::InteropLanguage::Java)) {
        return;
    }
    JavaDesugarManager desugarer{
        importManager, typeManager, diag, mangler, javagenOutputPath, outputPath, memberMap, pkg};

    if (!InteropLibBridge::IsInteropLibAccessible(importManager)) {
        return;
    }

    if (hasMirrorOrImpl) {
        AfterTypeCheckContext ctx{importManager, typeManager, pkg};
        desugarer.ProcessJavaMirrorImplStage(ctx, desugarPropRef);
    }

    // Currently CJMapping is enable by compile config --enable-interop-cjmapping
    if (targetInteropLanguage == GlobalOptions::InteropLanguage::Java) {
        auto nbegin = static_cast<uint8_t>(DesugarCJImplStage::BEGIN);
        auto nend = static_cast<uint8_t>(DesugarCJImplStage::END);
        for (uint8_t nstage = nbegin; nstage != nend; nstage++) {
            auto stage = static_cast<DesugarCJImplStage>(nstage);
            if (stage == DesugarCJImplStage::BEGIN) {
                continue;
            }
            if (stage == DesugarCJImplStage::PRE_GENERATE) {
                desugarer.GenerateTuplesGlueCode(pkg);
            }
            for (auto& file : pkg.files) {
                desugarer.ProcessCJImplStage(stage, *file);
            }
        }
    }
}

} // namespace Cangjie::Interop::Java
