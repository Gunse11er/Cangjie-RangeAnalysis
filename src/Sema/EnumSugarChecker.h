// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the EnumSugarChecker class.
 */

#ifndef CANGJIE_SEMA_ENUMSUGARCHECKER_H
#define CANGJIE_SEMA_ENUMSUGARCHECKER_H

#include <vector>

#include "EnumSugarTargetsFinder.h"

#include "TypeCheckerImpl.h"
#include "cangjie/AST/ASTContext.h"
#include "cangjie/AST/Node.h"

namespace Cangjie {
class TypeChecker::EnumSugarChecker {
public:
    EnumSugarChecker(TypeCheckerImpl& typeChecker, ASTContext& ctx, AST::RefExpr& re)
        : typeChecker(typeChecker),
          ctx(ctx),
          refExpr(re),
          enumSugarTargetsFinder(typeChecker.typeManager, ctx, re)
    {
    }
    /**
     * According to found targets, try to resolve enum sugar related targets.
     * @return the first of pair is true when error detected, and the second of pair is resolved targets.
     */
    std::pair<bool, std::vector<Ptr<AST::Decl>>> Resolve();

private:
    void CheckGenericEnumSugarWithTypeArgs(Ptr<AST::EnumDecl> ed);
    void CheckGenericEnumSugarWithoutTypeArgs(Ptr<const AST::EnumDecl> ed);
    Ptr<AST::Decl> CheckEnumSugarTargets();
    bool CheckVarDeclTargets();
    std::vector<Ptr<AST::Decl>> enumSugarTargets;
    TypeCheckerImpl& typeChecker;
    ASTContext& ctx;
    AST::RefExpr& refExpr;
    EnumSugarTargetsFinder enumSugarTargetsFinder;
};
} // namespace Cangjie

#endif
