// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares generate overflow APIs for codegen.
 */

#ifndef CANGJIE_OVDERFLOW_DISPATCHER_H
#define CANGJIE_OVDERFLOW_DISPATCHER_H

#include "llvm/IR/Value.h"

#include "Base/CHIRExprWrapper.h"
#include "CGModule.h"
#include "IRBuilder.h"

namespace Cangjie {
namespace CHIR {
class Type;
enum class ExprKind : uint8_t;
} // namespace CHIR
namespace CodeGen {
class IRBuilder2;

llvm::Value* GenerateOverflowWrappingArithmeticOp(IRBuilder2& irBuilder, const CHIR::ExprKind& kind,
    const CHIR::Type* ty, const std::vector<CGValue*>& argGenValues);
llvm::Value* GenerateOverflow(IRBuilder2& irBuilder, const OverflowStrategy& strategy, const CHIR::ExprKind& kind,
    const std::pair<const CHIR::IntType*, const CHIR::Type*>& tys, const std::vector<CGValue*>& argGenValues);

llvm::Value* GenerateOverflowApply(IRBuilder2& irBuilder, const CHIRIntrinsicWrapper& intrinsic);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_OVDERFLOW_DISPATCHER_H
