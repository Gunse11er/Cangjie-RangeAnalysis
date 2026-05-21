// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Define utils methods for generic instantiation.
 */

#ifndef CANGJIE_SEMA_GENERIC_INSTANTIATION_UTILS_H
#define CANGJIE_SEMA_GENERIC_INSTANTIATION_UTILS_H

#include <functional>

#include "cangjie/AST/Node.h"
#include "cangjie/Sema/TestManager.h"
namespace Cangjie {
/** Get @p decl 's sema type. If decl is extend decl, get it's extended sema type. */
inline Ptr<AST::Ty> GetDeclTy(const AST::Decl& decl)
{
    auto ty = decl.ty;
    if (decl.astKind == AST::ASTKind::EXTEND_DECL) {
        ty = static_cast<const AST::ExtendDecl&>(decl).extendedType->ty;
    }
    return ty;
}

inline Ptr<AST::Decl> GetOuterStructDecl(const AST::Decl& decl)
{
    auto outerDecl = decl.outerDecl;
    while (outerDecl && !outerDecl->IsNominalDecl()) {
        outerDecl = outerDecl->outerDecl;
    }
    return outerDecl;
}

/** Check if given @p decl is generic decl in generic structure declaration. */
inline bool IsGenericInGenericStruct(const AST::Decl& decl)
{
    auto outerDecl = GetOuterStructDecl(decl);
    return outerDecl && outerDecl->generic && outerDecl->IsNominalDecl() && decl.GetGeneric();
}

inline std::vector<Ptr<AST::Decl>> GetRealIndexingMembers(
    const std::vector<OwnedPtr<AST::Decl>>& decls, bool inGenericDecl = true)
{
    // NOTE: Filter primary constructors and members generated for test purpuses, for generic decls,
    // to get fixed members for indexing usage.
    std::vector<Ptr<AST::Decl>> ret;
    for (auto& member : decls) {
        if ((inGenericDecl && member->astKind == AST::ASTKind::PRIMARY_CTOR_DECL) ||
            TestManager::IsDeclGeneratedForTest(*member)
        ) {
            continue;
        }
        (void)ret.emplace_back(member.get());
    }
    return ret;
}

inline void WorkForMembers(AST::Decl& decl, const std::function<void(AST::Decl&)>& worker)
{
    if (decl.astKind == AST::ASTKind::PROP_DECL) {
        auto& pd = static_cast<AST::PropDecl&>(decl);
        std::for_each(pd.getters.begin(), pd.getters.end(), [&worker](auto& fd) { worker(*fd); });
        std::for_each(pd.setters.begin(), pd.setters.end(), [&worker](auto& fd) { worker(*fd); });
    } else {
        worker(decl);
    }
}

inline bool NeedSwitchContext(const AST::Decl& decl)
{
    auto outerDecl = GetOuterStructDecl(decl);
    return decl.IsNominalDecl() ||
        (outerDecl && outerDecl->IsNominalDecl() && (decl.GetGeneric() || decl.TestAttr(AST::Attribute::IMPORTED)));
}
} // namespace Cangjie
#endif
