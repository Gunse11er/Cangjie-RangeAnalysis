// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
/**
 * @file
 *
 * This file declares a class providing functions for promoting a subtype to a designated supertype if possible.
 * It also provides utility functions that handle type substitutions.
 */

#ifndef CANGJIE_SEMA_PROMOTION_H
#define CANGJIE_SEMA_PROMOTION_H

#include "cangjie/AST/Types.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie {
class Promotion {
public:
    explicit Promotion(TypeManager& tyMgr) : tyMgr(tyMgr)
    {
    }
    MultiTypeSubst GetPromoteTypeMapping(AST::Ty& from, AST::Ty& target);
    MultiTypeSubst GetDowngradeTypeMapping(AST::Ty& target, AST::Ty& upfrom);
    std::set<Ptr<AST::Ty>> Promote(AST::Ty& from, AST::Ty& target);
    // will return empty if any type arg of target (the subtype) unused in upfrom (the supertype)
    // e.g. downgrading to Future<T> from Any
    std::set<Ptr<AST::Ty>> Downgrade(AST::Ty& target, AST::Ty& upfrom);

private:
    TypeManager& tyMgr;
    std::set<Ptr<AST::Ty>> PromoteHandleIdealTys(AST::Ty& from, AST::Ty& target) const;
    std::set<Ptr<AST::Ty>> PromoteHandleFunc(AST::Ty& from, AST::Ty& target);
    std::set<Ptr<AST::Ty>> PromoteHandleTuple(AST::Ty& from, AST::Ty& target);
    std::set<Ptr<AST::Ty>> PromoteHandleTyVar(AST::Ty& from, AST::Ty& target);
    std::set<Ptr<AST::Ty>> PromoteHandleNominal(AST::Ty& from, const AST::Ty& target);
};
} // namespace Cangjie
#endif // CANGJIE_SEMA_PROMOTION_H
