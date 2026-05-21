// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_VARRAYEXPRIMPL_H
#define CANGJIE_VARRAYEXPRIMPL_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CHIR {
class VArray;
class VArrayBuilder;
} // namespace CHIR
namespace CodeGen {
class IRBuilder2;
llvm::Value* GenerateVArray(IRBuilder2& irBuilder, const CHIR::VArray& varray);
llvm::Value* GenerateVArrayBuilder(IRBuilder2& irBuilder, const CHIR::VArrayBuilder& varrayBuilder);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_VARRAYEXPRIMPL_H
