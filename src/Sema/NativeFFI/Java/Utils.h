// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares auxiliary methods for java interop implementation
 */
#ifndef CANGJIE_SEMA_NATIVE_FFI_JAVA_UTILS
#define CANGJIE_SEMA_NATIVE_FFI_JAVA_UTILS

#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"

namespace Cangjie::Native::FFI::Java {
const Ptr<AST::ClassDecl> GetSyntheticClass(const ImportManager& importManager, const AST::ClassLikeDecl& cld);

std::string GetMirrorReferenceWrapperNameFromClassLike(const AST::ClassLikeDecl &mirror);

std::string GetImplRegistryCompanionClassName(const AST::ClassLikeDecl& javaImplDecl);

std::vector<Ptr<AST::ClassLikeDecl>> GetJavaMirrors(AST::File& file);

std::vector<Ptr<AST::ClassLikeDecl>> GetJavaMirrors(AST::Package& pkg);

std::vector<Ptr<AST::ClassDecl>> GetJavaImpls(AST::File& file);

std::vector<Ptr<AST::ClassDecl>> GetJavaImpls(AST::Package& pkg);

std::vector<Ptr<AST::ClassDecl>> GetJavaImplRegistryCompanions(AST::File& file);

std::vector<Ptr<AST::ClassDecl>> GetJavaImplRegistryCompanions(AST::Package& pkg);

bool IsJavaImplRegistryCompanionReferenceField(const AST::Node& node);

} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_NATIVE_FFI_JAVA_UTILS
