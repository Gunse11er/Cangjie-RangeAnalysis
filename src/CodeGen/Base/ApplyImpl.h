// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements codegen for CHIR Apply.
 */
#ifndef CANGJIE_APPLY_H
#define CANGJIE_APPLY_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CodeGen {
class IRBuilder2;
class CHIRApplyWrapper;
llvm::Value* GenerateApply(IRBuilder2& irBuilder, const CHIRApplyWrapper& apply);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_APPLY_H
