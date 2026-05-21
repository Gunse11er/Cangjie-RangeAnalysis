// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_EMITFUNCTIONIR_H
#define CANGJIE_EMITFUNCTIONIR_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CHIR {
class RawArrayInitByValue;
}
namespace CodeGen {
class IRBuilder2;
class CHIRRawArrayAllocateWrapper;
llvm::Value* GenerateRawArrayAllocate(IRBuilder2& irBuilder, const CHIRRawArrayAllocateWrapper& rawArray);
llvm::Value* GenerateRawArrayInitByValue(IRBuilder2& irBuilder, const CHIR::RawArrayInitByValue& rawArrayInitByValue);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_TYPECASTIMPL_H