// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_PARSE_PARSE_MODIFIERS_RULES_H
#define CANGJIE_PARSE_PARSE_MODIFIERS_RULES_H

#include "Parser.h"

namespace Cangjie {

struct ConflictArray {
    const TokenKind* data;
    size_t size;
};
enum class DefKind : uint8_t;

bool HasScopeRules(DefKind defKind, ScopeKind scopeKind);
bool IsScopeRulesEmpty(DefKind defKind, ScopeKind scopeKind);
bool IsModifierAllowed(DefKind defKind, ScopeKind scopeKind, TokenKind modifier);
ConflictArray GetConflictingModifiers(DefKind defKind, ScopeKind scopeKind, TokenKind modifier);
bool HasWarningRules(DefKind defKind, ScopeKind scopeKind);
ConflictArray GetWarningConflicts(DefKind defKind, ScopeKind scopeKind, TokenKind modifier);
std::optional<AST::Attribute> GetAttributeByModifier(TokenKind tokenKind);

} // namespace Cangjie
#endif // CANGJIE_PARSE_PARSEMODIFIERS_H
