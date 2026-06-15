// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares auxiliary methods for interop implementation
 */
#ifndef CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPE_CHECK_UTILS
#define CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPE_CHECK_UTILS

#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"

using namespace Cangjie::AST;

namespace Cangjie::Native::FFI::Java {

OwnedPtr<AST::ClassDecl> CloneClassSkeleton(AST::ClassLikeDecl& sample, std::string&& name);

void InsertMethodStub(AST::FuncDecl& fd, const ImportManager& importManager, TypeManager& typeManager);

AST::ClassDecl& GetExceptionDecl(const ImportManager& importManager);

} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_NATIVE_FFI_JAVA_BEFORE_TYPE_CHECK_UTILS
