// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"

#include "NativeFFI/Java/JavaCodeGenerator/JavaSourceCodeGenerator.h"
#include "NativeFFI/Utils.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::Interop::Java {
using namespace Cangjie::Native::FFI;

void JavaDesugarManager::GenerateJavaSourceCode(AfterTypeCheckContext& ctx)
{
    for (auto& decl : ctx.GetJavaImplReferenceWrappers()) {
        const std::string fileJ = decl->identifier.Val() + ".java";
        auto codegen = JavaSourceCodeGenerator(decl, mangler, typeManager, javaCodeGenPath, fileJ,
            GetCangjieLibName(outputLibPath, decl->GetFullPackageName()), {});
        CJC_ASSERT(&codegen);
        codegen.Generate();
    }
}

} // namespace Cangjie::Interop::Java