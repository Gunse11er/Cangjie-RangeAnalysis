// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the JavaMangler used to AST Writer.
 */

#ifndef CANGJIE_MODULES_ASTSERIALIZATION_JAVA_MANGLER_H
#define CANGJIE_MODULES_ASTSERIALIZATION_JAVA_MANGLER_H

#include "cangjie/AST/Types.h"
#include "cangjie/Mangle/BaseMangler.h"

namespace Cangjie {
bool ContainJavaGenerics(const AST::Ty& ty)
{
    if (!AST::Ty::IsTyCorrect(&ty)) {
        return false;
    }
    if (AST::IsJClassOrInterface(ty) && !ty.typeArgs.empty()) {
        return true;
    }
    for (auto tyArg : ty.typeArgs) {
        if (ContainJavaGenerics(*tyArg)) {
            return true;
        }
    }
    return false;
}

class JavaMangler : public BaseMangler {
public:
    bool NeedRemangle(const AST::FuncDecl& funcDecl) const override
    {
        bool isMemberFunc = funcDecl.outerDecl &&
            funcDecl.TestAnyAttr(AST::Attribute::IN_CLASSLIKE, AST::Attribute::IN_ENUM, AST::Attribute::IN_STRUCT,
                AST::Attribute::IN_EXTEND);
        return exportIdMode &&
            (ContainJavaGenerics(*funcDecl.ty) || (isMemberFunc && ContainJavaGenerics(*funcDecl.outerDecl->ty)));
    }
};
} // namespace Cangjie
#endif
