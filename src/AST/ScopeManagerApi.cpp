// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements ScopeManager apis.
 */

#include "cangjie/AST/ScopeManagerApi.h"

#include "cangjie/AST/ASTContext.h"

using namespace Cangjie;
using namespace AST;

Symbol* ScopeManagerApi::GetScopeGate(const ASTContext& ctx, const std::string& scopeName)
{
    auto it = ctx.invertedIndex.scopeGateMap.find(scopeName);
    if (it != ctx.invertedIndex.scopeGateMap.end()) {
        return it->second;
    }
    return nullptr;
}

std::string ScopeManagerApi::GetScopeGateName(const std::string& scopeNameOrGateName)
{
    std::string currentScope;
    auto found = scopeNameOrGateName.find_last_of(childScopeNameSplit);
    if (found != std::string::npos) {
        // e.g. a0a_a -> a0a (intermediate step: remove '_' suffix)
        //      Then a0a -> a_a (final: convert to scope gate name)
        //      If input is a scope gate name, returns parent scope gate name.
        //      If input is a scope name, returns current scope gate name.
        currentScope = scopeNameOrGateName.substr(0, found);
    } else {
        currentScope = scopeNameOrGateName;
    }
    found = currentScope.find_last_of(scopeNameSplit);
    if (found != std::string::npos) {
        // e.g. a0a -> a_a
        currentScope.replace(found, 1, 1, childScopeNameSplit);
    } else {
        // Toplevel don't have root name.
        return "";
    }
    return currentScope;
}
