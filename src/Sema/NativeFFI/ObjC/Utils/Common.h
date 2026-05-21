// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares common utils for Cangjie <-> Objective-C interop.
 * Needs to be structured properly in the future.
 */

#ifndef CANGJIE_SEMA_OBJ_C_UTILS_COMMON_H
#define CANGJIE_SEMA_OBJ_C_UTILS_COMMON_H

#include "InheritanceChecker/MemberSignature.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Utils/SafePointer.h"

namespace Cangjie::Interop::ObjC {

/**
 *  Returns native handle field declaration for `target` mirror/impl declaration
 */
Ptr<AST::VarDecl> GetNativeVarHandle(const AST::ClassDecl& target);
bool HasMirrorSuperClass(const AST::ClassLikeDecl& target);
Ptr<AST::Decl> FindMirrorMember(const std::string_view& mirrorMemberIdent, const AST::InheritableDecl& target);
bool IsStaticInitMethod(const AST::Node& node);
bool HasMirrorSuperInterface(const AST::ClassLikeDecl& target);
Ptr<AST::ClassDecl> GetImplSuperClass(const AST::ClassDecl& target);
bool HasImplSuperClass(const AST::ClassDecl& target);
Ptr<AST::FuncDecl> GetNativeHandleGetter(const AST::ClassLikeDecl& target);
Ptr<AST::ClassDecl> GetSyntheticWrapper(const ImportManager& importManager, const AST::ClassLikeDecl& target);
Ptr<AST::FuncDecl> GetFinalizer(const AST::ClassDecl& target);
bool IsSyntheticWrapper(const AST::Decl& decl);
void GenerateSyntheticClassAbstractMemberImplStubs(AST::ClassDecl& synthetic, const MemberMap& members);

} // namespace Cangjie::Interop::ObjC

#endif // CANGJIE_SEMA_OBJ_C_UTILS_COMMON_H
