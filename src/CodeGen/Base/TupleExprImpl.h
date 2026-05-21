// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_TUPLE_EXPR_IMPL_H
#define CANGJIE_TUPLE_EXPR_IMPL_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CHIR {
class Tuple;
}
namespace CodeGen {
class IRBuilder2;
llvm::Value* GenerateNativeTuple(IRBuilder2& irBuilder, const CHIR::Tuple& tuple);
llvm::Value* GenerateEnum(IRBuilder2& irBuilder, const CHIR::Tuple& tuple);
llvm::Value* GenerateStruct(IRBuilder2& irBuilder, const CHIR::Tuple& tuple);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_TUPLE_EXPR_IMPL_H
