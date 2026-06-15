// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GenerateJavaImplRegistryCompanion.h"
#include "JavaInteropManager.h"
#include "NativeFFI/Java/BeforeTypeCheck/Utils.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Utils/SafePointer.h"
#include "cangjie/AST/Utils.h"
#include "NativeFFI/Java/Utils.h"
#include "cangjie/Utils/CheckUtils.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;
using namespace Cangjie::Native::FFI;
using namespace std;

namespace Cangjie::Native::FFI::Java {

OwnedPtr<AST::ClassDecl> GenerateJavaImplRegistryCompanionClass::GenerateRegistryCompanion(AST::ClassDecl& impl) const
{
    CJC_ASSERT(IsImpl(impl));
    auto regCompanion = CloneClassSkeleton(impl, GetImplRegistryCompanionClassName(impl));
    regCompanion->EnableAttr(Attribute::JAVA_IMPL_REGISTRY_COMPANION);

    return regCompanion;
}

void GenerateJavaImplRegistryCompanionClass::Process(PreTypeCheckContext& ctx)
{
    for (auto impl : ctx.javaImpls) {
        ctx.AddGeneratedDecl(GenerateRegistryCompanion(*impl));
    }
}

} // Cangjie::Native::FFI::Java
