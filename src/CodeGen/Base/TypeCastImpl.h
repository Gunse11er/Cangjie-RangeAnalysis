// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_TYPECASTIMPL_H
#define CANGJIE_TYPECASTIMPL_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CHIR {
class Type;
}
namespace CodeGen {
class CGType;
class CGValue;
class IRBuilder2;
class CGValue;
class CHIRTypeCastWrapper;
llvm::Value* GenerateTypeCast(IRBuilder2& irBuilder, const CHIRTypeCastWrapper& typeCast);
llvm::Value* GenerateTupleTypeCast(IRBuilder2& irBuilder, const CGValue& srcCGValue, const CGType& targetCGType);
llvm::Value* GenerateGenericTypeCast(IRBuilder2& irBuilder, const CGValue& cgSrcValue, const CHIR::Type& srcTy,
    const CHIR::Type& targetTy);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_TYPECASTIMPL_H
