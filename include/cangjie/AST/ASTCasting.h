// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements type casting ast related nodes.
 */

#ifndef CANGJIE_UTILS_AST_CASTING_H
#define CANGJIE_UTILS_AST_CASTING_H

#include <type_traits>

#include "cangjie/AST/NodeX.h"
#include "cangjie/AST/Types.h"
#include "cangjie/Utils/CastingTemplate.h"

namespace Cangjie {
// Register NodeType::kind for mono typed AST.
#define ASTKIND(KIND, VALUE, NODE, SIZE)                                                                               \
    template <> struct NodeType<AST::NODE> {                                                                           \
        static const AST::ASTKind kind = AST::ASTKind::KIND;                                                           \
    };
#include "cangjie/AST/ASTKind.inc"
#undef ASTKIND

// Register NodeType::kind for mono typed Ty.
DEFINE_NODE_TYPE_KIND(AST::ArrayTy, AST::TypeKind::TYPE_ARRAY);
DEFINE_NODE_TYPE_KIND(AST::VArrayTy, AST::TypeKind::TYPE_VARRAY);
DEFINE_NODE_TYPE_KIND(AST::PointerTy, AST::TypeKind::TYPE_POINTER);
DEFINE_NODE_TYPE_KIND(AST::CStringTy, AST::TypeKind::TYPE_CSTRING);
DEFINE_NODE_TYPE_KIND(AST::TupleTy, AST::TypeKind::TYPE_TUPLE);
DEFINE_NODE_TYPE_KIND(AST::FuncTy, AST::TypeKind::TYPE_FUNC);
DEFINE_NODE_TYPE_KIND(AST::UnionTy, AST::TypeKind::TYPE_UNION);
DEFINE_NODE_TYPE_KIND(AST::IntersectionTy, AST::TypeKind::TYPE_INTERSECTION);
DEFINE_NODE_TYPE_KIND(AST::InterfaceTy, AST::TypeKind::TYPE_INTERFACE);
DEFINE_NODE_TYPE_KIND(AST::ClassTy, AST::TypeKind::TYPE_CLASS);
DEFINE_NODE_TYPE_KIND(AST::EnumTy, AST::TypeKind::TYPE_ENUM);
DEFINE_NODE_TYPE_KIND(AST::StructTy, AST::TypeKind::TYPE_STRUCT);
DEFINE_NODE_TYPE_KIND(AST::TypeAliasTy, AST::TypeKind::TYPE);
DEFINE_NODE_TYPE_KIND(AST::GenericsTy, AST::TypeKind::TYPE_GENERICS);

template <typename To>
using ShouldImplSeparately = std::enable_if_t<std::is_base_of_v<AST::Node, To> &&
    ShouldInstantiate<To, AST::Decl, AST::InheritableDecl, AST::VarDeclAbstract, AST::VarDecl, AST::ClassLikeDecl,
        AST::Pattern, AST::Type, AST::Expr, AST::Node, AST::OverloadableExpr, AST::NameReferenceExpr>::value>;

#ifndef UT
#define STATIC_ASSERT static_assert
#endif

// Defined the mono type checking method for ASTNode, Ty.
template <typename To> struct TypeAs<To, ShouldImplSeparately<To>> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        STATIC_ASSERT(NodeType<To>::kind != AST::ASTKind::NODE);
        return node.astKind == NodeType<To>::kind;
    }
};

template <typename To> struct TypeAs<To, std::enable_if_t<std::is_base_of_v<AST::Ty, To>>> {
    static inline bool IsInstanceOf(const AST::Ty& ty)
    {
        return ty.kind == NodeType<To>::kind;
    }
};

// Customized type checking function for ASTNode.
template <> struct TypeAs<AST::Decl> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::DECL && node.astKind <= AST::ASTKind::INVALID_DECL;
    }
};

template <> struct TypeAs<AST::InheritableDecl> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::CLASS_LIKE_DECL && node.astKind <= AST::ASTKind::STRUCT_DECL;
    }
};

template <> struct TypeAs<AST::VarDecl> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::VAR_DECL && node.astKind <= AST::ASTKind::FUNC_PARAM;
    }
};

template <> struct TypeAs<AST::FuncParam> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind == AST::ASTKind::FUNC_PARAM || node.astKind == AST::ASTKind::MACRO_EXPAND_PARAM;
    }
};

template <> struct TypeAs<AST::VarDeclAbstract> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return TypeAs<AST::VarDecl>::IsInstanceOf(node) || node.astKind == AST::ASTKind::VAR_WITH_PATTERN_DECL;
    }
};

template <> struct TypeAs<AST::ClassLikeDecl> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::CLASS_LIKE_DECL && node.astKind <= AST::ASTKind::INTERFACE_DECL;
    }
};

template <> struct TypeAs<AST::Pattern> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::PATTERN && node.astKind <= AST::ASTKind::INVALID_PATTERN;
    }
};

template <> struct TypeAs<AST::Type> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::TYPE && node.astKind <= AST::ASTKind::INVALID_TYPE;
    }
};

template <> struct TypeAs<AST::Expr> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::EXPR && node.astKind <= AST::ASTKind::INVALID_EXPR;
    }
};

template <> struct TypeAs<AST::OverloadableExpr> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind >= AST::ASTKind::ASSIGN_EXPR && node.astKind <= AST::ASTKind::SUBSCRIPT_EXPR;
    }
};

template <> struct TypeAs<AST::NameReferenceExpr> {
    static inline bool IsInstanceOf(const AST::Node& node)
    {
        return node.astKind == AST::ASTKind::MEMBER_ACCESS || node.astKind == AST::ASTKind::REF_EXPR;
    }
};

// Customized type checking function for Ty.
template <> struct TypeAs<AST::ClassLikeTy> {
    static inline bool IsInstanceOf(const AST::Ty& ty)
    {
        return ty.kind == AST::TypeKind::TYPE_CLASS || ty.kind == AST::TypeKind::TYPE_INTERFACE;
    }
};

template <> struct TypeAs<AST::PrimitiveTy> {
    static inline bool IsInstanceOf(const AST::Ty& ty)
    {
        return ty.IsPrimitive();
    }
};

template <> struct TypeAs<AST::ClassThisTy> {
    static inline bool IsInstanceOf(const AST::Ty& ty)
    {
        if (ty.kind != AST::TypeKind::TYPE_CLASS) {
            return false;
        }
        return dynamic_cast<const AST::ClassThisTy*>(&ty) != nullptr;
    }
};

template <> struct TypeAs<AST::RefEnumTy> {
    static inline bool IsInstanceOf(const AST::Ty& ty)
    {
        if (ty.kind != AST::TypeKind::TYPE_ENUM) {
            return false;
        }
        return dynamic_cast<const AST::RefEnumTy*>(&ty) != nullptr;
    }
};
} // namespace Cangjie

#endif // CANGJIE_UTILS_AST_CASTING_H