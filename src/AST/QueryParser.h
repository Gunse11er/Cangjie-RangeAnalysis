// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the QueryParser.
 */

#ifndef CANGJIE_AST_QUERY_PARSER_H
#define CANGJIE_AST_QUERY_PARSER_H

#include <optional>
#include <string>

#include "cangjie/AST/Query.h"
#include "cangjie/Parse/Parser.h"

namespace Cangjie {
/**
 * @brief The main class used for parsing query statements.
 */
class QueryParser : public Parser {
public:
    template <typename... Args> explicit QueryParser(Args&&... args) : Parser(std::forward<Args>(args)...)
    {
    }
    /**
     * @brief The main parse entry.
     */
    std::unique_ptr<Query> Parse();

private:
    /**
     * @brief Parse boolean clause with parens, like (a:b || c:d).
     */
    std::unique_ptr<Query> ParseParenClause();
    /**
     * @brief Parse boolean clause, like a:b && c:d.
     */
    std::unique_ptr<Query> ParseBooleanClause();
    /**
     * @brief Parse normal term, like a:b.
     */
    std::unique_ptr<Query> ParseNormalTerm();
    /**
     * @brief Parse position term, like _ < (1,2,3).
     */
    std::unique_ptr<Query> ParsePositionTerm();
    /**
     * @brief Parse comparator sign, '=', '>', '>=', '<', '<='.
     */
    std::optional<std::string> ParseComparator();
    /**
     * @brief Parse term entry.
     */
    std::unique_ptr<Query> ParseTerm();

    bool parsingParenClause{false};
};
} // namespace Cangjie

#endif
