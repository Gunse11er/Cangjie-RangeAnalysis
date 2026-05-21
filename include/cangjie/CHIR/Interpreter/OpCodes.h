// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the bytecode operation codes for the BCHIR interpreter.
 */

#ifndef CANGJIE_CHIR_INTERPRETER_OPCODES_H
#define CANGJIE_CHIR_INTERPRETER_OPCODES_H

#include "cangjie/Utils/Utils.h"

namespace Cangjie::CHIR::Interpreter {

enum class OpCode {
#define OPCODE(ID, VALUE, SIZE, HAS_EXC_HANDLER) ID,
#include "cangjie/CHIR/Interpreter/OpCodes.inc"
#undef OPCODE
};

const std::string OpCodeLabel[static_cast<size_t>(OpCode::INVALID) + 1]{
#define OPCODE(ID, VALUE, SIZE, HAS_EXC_HANDLER) (VALUE),
#include "cangjie/CHIR/Interpreter/OpCodes.inc"
#undef OPCODE
};

const uint32_t OpCodeArgSize[static_cast<size_t>(OpCode::INVALID) + 1]{
#define OPCODE(ID, VALUE, SIZE, HAS_EXC_HANDLER) (SIZE),
#include "cangjie/CHIR/Interpreter/OpCodes.inc"
#undef OPCODE
};

constexpr bool OpHandlesException[static_cast<size_t>(OpCode::INVALID) + 1]{
#define OPCODE(ID, VALUE, SIZE, HAS_EXC_HANDLER) (HAS_EXC_HANDLER),
#include "cangjie/CHIR/Interpreter/OpCodes.inc"
#undef OPCODE
};

uint32_t inline GetOpCodeArgSize(OpCode opCode)
{
    return OpCodeArgSize[static_cast<size_t>(opCode)];
}

std::string inline GetOpCodeLabel(OpCode opCode)
{
    return OpCodeLabel[static_cast<size_t>(opCode)];
}

constexpr bool inline OpHasExceptionHandler(OpCode opCode)
{
    return OpHandlesException[static_cast<size_t>(opCode)];
}

}; // namespace Cangjie::CHIR::Interpreter

#endif // CANGJIE_CHIR_INTERPRETER_OPCODES_H
