// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
* @file
*
* This file declares the method of determining the reference type.
*/

#ifndef CANGJIE_AST_REFERENCETYPE_H
#define CANGJIE_AST_REFERENCETYPE_H

#include "cangjie/AST/Types.h"

namespace Cangjie {
namespace AST {
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
inline bool IsReferenceType(const AST::Ty& ty)
{
    // With CJNATIVE-BE, we translate
    // 1) Classes,
    // 2) Option<Ref>,
    // 3) Arrays
    // as reference types.
    bool isRefOption = false;

    if (ty.IsCoreOptionType()) {
        auto& enumTy = static_cast<const AST::EnumTy&>(ty);
        auto elemTy = enumTy.typeArgs[0];
        bool elemTyIsOption = elemTy->IsCoreOptionType();
        // notice that Option<Option<Ref>> is not reference type.
        isRefOption = IsReferenceType(*elemTy) && !elemTyIsOption;
    }
    return ty.IsClassLike() || ty.IsArray() || isRefOption;
}
#endif
} // namespace AST
} // namespace Cangjie
#endif // CANGJIE_AST_REFERENCETYPE_H
