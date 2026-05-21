// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements a translation from CHIR to BCHIR.
 */
#include "cangjie/CHIR/Interpreter/CHIR2BCHIR.h"
#include "cangjie/CHIR/Interpreter/Utils.h"

using namespace Cangjie::CHIR;
using namespace Interpreter;

void CHIR2BCHIR::TranslateBinaryExpression(Context& ctx, const Expression& expr)
{
    CJC_ASSERT(expr.GetNumOfOperands() == Bchir::FLAG_TWO);
    auto binaryExpression = StaticCast<const BinaryExpression*>(&expr);
    auto opCode = Cangjie::CHIR::Interpreter::BinExprKind2OpCode(expr.GetExprKind());
    auto typeKind = binaryExpression->GetOperand(0)->GetType()->GetTypeKind();
    auto overflowStrat = static_cast<Bchir::ByteCodeContent>(binaryExpression->GetOverflowStrategy());
    PushOpCodeWithAnnotations<false, true>(ctx, opCode, expr, typeKind, overflowStrat);
    if (opCode == OpCode::BIN_LSHIFT || opCode == OpCode::BIN_RSHIFT) {
        ctx.def.Push(static_cast<Bchir::ByteCodeContent>(binaryExpression->GetOperand(1)->GetType()->GetTypeKind()));
    }
}