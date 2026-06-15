// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPECHECK_PRETYPECHECK
#define CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPECHECK_PRETYPECHECK

#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"

namespace Cangjie::Native::FFI::Java {
void PrepareTypeCheck(AST::Package& pkg, const ImportManager& importManager, TypeManager& typeManager);
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPECHECK_PRETYPECHECK
