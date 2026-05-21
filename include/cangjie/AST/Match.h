// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares AST match apis, and some useful macros.
 */

#ifndef CANGJIE_AST_MATCH_H
#define CANGJIE_AST_MATCH_H

#include "cangjie/AST/NodeX.h"
#include "cangjie/AST/ASTCasting.h"

namespace Cangjie::AST {
/**
 * ASTKind to Node type mapping.
 */
template <ASTKind Kind> struct NodeKind {};
#define ASTKIND(KIND, VALUE, NODE, SIZE)                                                                               \
    template <> struct NodeKind<ASTKind::KIND> {                                                                       \
        using Type = AST::NODE;                                                                                        \
    };
#include "cangjie/AST/ASTKind.inc"
#undef ASTKIND

/**
 * Convert Node to certain ASTKind, use static_cast as possible.
 * @param node Node to be convert.
 * @return The AST Node of kind @p Kind.
 */
template <ASTKind Kind> auto As(Ptr<Node> node)
{
    return DynamicCast<typename NodeKind<Kind>::Type*>(node.get());
}

/**
 * Convert Node to certain ASTKind, use static_cast.
 * @param node Node to be convert.
 * @return The AST Node of kind @p Kind.
 */
template <ASTKind Kind, typename NodeT> inline auto StaticAs(NodeT node)
{
    return StaticCast<typename NodeKind<Kind>::Type*>(node);
}
} // namespace Cangjie::AST
#endif // CANGJIE_AST_MATCH_H
