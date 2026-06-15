// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RewriteJavaMirrorFields.h"
#include "JavaInteropManager.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Utils.h"

using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;
using namespace Cangjie::Native::FFI;
using namespace std;

namespace Cangjie::Native::FFI::Java {

void RewriteJavaMirrorFields::Process(PreTypeCheckContext& ctx)
{
    for (auto mirror : ctx.javaMirrors) {
        if (auto mirrorClass = As<ASTKind::CLASS_DECL>(mirror)) {
            InsertMirrorVarProp(*mirrorClass, Attribute::JAVA_MIRROR);
        }
    }
}

} // Cangjie::Native::FFI::Java
