// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Query and related classes.
 */

#ifndef CANGJIE_AST_QUERY_H
#define CANGJIE_AST_QUERY_H

#include <memory>
#include <string>
#include <unordered_set>

#include "cangjie/Basic/Position.h"

namespace Cangjie {
/**
 * Operations on the Query node.
 */
enum class Operator {
    AND, /**< Condition1 && Condition2. */
    OR,  /**< Condition1 || Condition2. */
    NOT, /**< Condition1 !  Condition2. */
};

/**
 * The type of the Query node.
 */
enum class QueryType { OP, STRING, POS, NONE };

/**
 * Search term match kind.
 */
enum class MatchKind {
    PRECISE, /**< Query string "name: foo". */
    PREFIX,  /**< Query string "name: foo*". */
    SUFFIX   /**< Query string "name: *foo". */
};

/**
 * Query is a query tree, Leaf Node represent the query, none leaf node is query condition.
 */
struct Query {
    Query(std::string key, std::string value) : key(std::move(key)), value(std::move(value))
    {
    }
    Query(std::string key, std::string value, MatchKind matchKind)
        : key(std::move(key)), value(std::move(value)), matchKind(matchKind)
    {
    }
    explicit Query(Operator op) : op(op)
    {
        type = QueryType::OP;
    }
    Query() = default;
    std::string key;                         /**< Leaf node's key. */
    std::string value;                       /**< Leaf node's value. */
    std::unordered_set<uint64_t> fileHashes; /**< For filter certain files. */
    Position pos;                            /**< Save the Position value. */
    std::string sign{"="};                   /**< Only position filed support '=', '<', '<='. */
    QueryType type{QueryType::NONE};
    std::unique_ptr<Query> left;
    std::unique_ptr<Query> right;
    Operator op{Operator::AND};
    MatchKind matchKind{MatchKind::PRECISE};
    void PrettyPrint(std::string& result) const;
};
} // namespace Cangjie

#endif
