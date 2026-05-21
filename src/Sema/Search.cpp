// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements Search apis for TypeChecker.
 */

#include <thread>

#include "TypeCheckerImpl.h"
#include "cangjie/AST/ASTContext.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Query.h"
#include "cangjie/AST/ScopeManagerApi.h"
#include "cangjie/AST/Searcher.h"
#include "cangjie/AST/Symbol.h"
#include "cangjie/Utils/Utils.h"
#include "cangjie/Utils/ProfileRecorder.h"

using namespace Cangjie;
using namespace AST;

namespace {
constexpr unsigned int CORES_REQUIRED_WARMUP = 8;
}

void TypeChecker::TypeCheckerImpl::WarmupCache(const ASTContext& ctx) const
{
    Utils::ProfileRecorder recorder("PrepareTypeCheck", "WarmupCache");
    auto numProcessors = std::thread::hardware_concurrency();
    if (numProcessors < CORES_REQUIRED_WARMUP) {
        return;
    }
    auto scopeNames = ctx.searcher->GetScopeNamesByPrefix(ctx, TOPLEVEL_SCOPE_NAME);
    auto numScopeNames = scopeNames.size();
    if (numProcessors < numScopeNames) {
        return;
    }
    std::unordered_map<std::string, std::vector<Symbol*>> cache;
    std::vector<std::thread> threads(numScopeNames);
    std::vector<std::unique_ptr<Searcher>> searchers(numScopeNames);
    for (size_t i = 0; i < numScopeNames; i++) {
        searchers[i] = std::make_unique<Searcher>();
    }
    for (size_t i = 0; i < numScopeNames; i++) {
        std::string scopeName = scopeNames[i];
        Searcher* s = searchers[i].get();
        threads[i] = std::thread([scopeName, s, &ctx]() {
            Query q(Operator::NOT);
            q.left = std::make_unique<Query>(Operator::AND);
            q.left->left = std::make_unique<Query>("scope_name", scopeName);
            q.left->right = std::make_unique<Query>(Operator::OR);
            q.left->right->left = std::make_unique<Query>("ast_kind", "decl");
            q.left->right->left->matchKind = MatchKind::SUFFIX;
            q.left->right->right = std::make_unique<Query>("ast_kind", "func_param");
            q.right = std::make_unique<Query>("ast_kind", "extend_decl");
            s->Search(ctx, &q);
        });
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    for (auto& searcher : searchers) {
        cache.merge(searcher->GetCache());
    }
    ctx.searcher->SetCache(cache);
}

std::vector<Symbol*> TypeChecker::TypeCheckerImpl::GetToplevelDecls(const ASTContext& ctx) const
{
    // "scope_level:0 && ast_kind: *decl"
    Query q(Operator::AND);
    q.left = std::make_unique<Query>("scope_level", "0");
    q.right = std::make_unique<Query>("ast_kind", "decl");
    q.right->matchKind = MatchKind::SUFFIX;
    return ctx.searcher->Search(ctx, &q, Sort::posAsc);
}

std::vector<Symbol*> TypeChecker::TypeCheckerImpl::GetAllDecls(const ASTContext& ctx) const
{
    Query q("ast_kind", "decl", MatchKind::SUFFIX);
    return ctx.searcher->Search(ctx, &q, Sort::posAsc);
}

std::vector<Symbol*> TypeChecker::TypeCheckerImpl::GetGenericCandidates(const ASTContext& ctx) const
{
    return ctx.searcher->Search(ctx,
        "ast_kind : class_decl || ast_kind : interface_decl || ast_kind : struct_decl || ast_kind : enum_decl || "
        "ast_kind : func_decl || ast_kind : extend_decl || ast_kind : builtin_decl",
        Sort::posAsc);
}

std::vector<Symbol*> TypeChecker::TypeCheckerImpl::GetAllStructDecls(const ASTContext& ctx) const
{
    return ctx.searcher->Search(ctx,
        "ast_kind : class_decl || ast_kind : interface_decl || ast_kind : struct_decl || ast_kind : enum_decl || "
        "ast_kind : extend_decl",
        Sort::posAsc);
}

std::vector<Symbol*> TypeChecker::TypeCheckerImpl::GetSymsByASTKind(
    const ASTContext& ctx, ASTKind astKind, const Order& order) const
{
    Query q = Query("ast_kind", ASTKIND_TO_STRING_MAP[astKind]);
    return ctx.searcher->Search(ctx, &q, order);
}
