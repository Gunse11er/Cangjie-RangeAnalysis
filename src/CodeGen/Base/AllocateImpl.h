// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements codegen for CHIR Allocate.
 */
#ifndef CANGJIE_ALLOCATEIMPL_H
#define CANGJIE_ALLOCATEIMPL_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CodeGen {
class IRBuilder2;
class CHIRAllocateWrapper;
llvm::Value* GenerateAllocate(IRBuilder2& irBuilder, const CHIRAllocateWrapper& alloca);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_ALLOCATEIMPL_H