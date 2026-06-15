// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "PreTypeCheck.h"
#include "JavaInteropManager.h"

namespace Cangjie::Native::FFI::Java {
using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;

void PrepareTypeCheck(Package& pkg, const ImportManager& importManager, TypeManager& typeManager)
{
    JavaInteropManager jim(importManager, typeManager);
    jim.Process(pkg);
}
} // namespace Cangjie::Native::FFI::Java
