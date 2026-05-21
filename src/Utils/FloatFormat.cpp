// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements float format related apis.
 */

#include "cangjie/Utils/FloatFormat.h"

#include <cmath>
#include <sstream>

namespace {
constexpr uint32_t FLOAT32_SIGN_MASK = 0x80000000;
constexpr uint32_t FLOAT32_EXP_MASK = 0x7F800000;
constexpr uint32_t FLOAT32_TAIL_MASK = 0x007FFFFF;
constexpr uint32_t FLOAT32_EXP_BASE = 0b01111111;
constexpr uint32_t FLOAT32_TAIL_WIDTH = 23;
constexpr uint16_t FLOAT16_SIGN_MASK = 0x8000;
constexpr uint16_t FLOAT16_EXP_BASE = 0b01111;
constexpr uint16_t FLOAT16_EXP_MAX = 0b11110;
constexpr uint16_t FLOAT16_TAIL_WIDTH = 10;
constexpr uint16_t FLOAT16_INF = 0b11111 << FLOAT16_TAIL_WIDTH;
constexpr uint16_t FLOAT32_FLOAT16_TAIL_OFFSET = FLOAT32_TAIL_WIDTH - FLOAT16_TAIL_WIDTH;
}

namespace Cangjie::FloatFormat {
uint16_t Float32ToFloat16(float value)
{
    uint16_t result = 0;
    // We need the original bit field of float, thus we can not use `static_cast` here.
    uint32_t bit32 = *reinterpret_cast<uint32_t*>(&value);
    int32_t exp = static_cast<int32_t>(((bit32 & FLOAT32_EXP_MASK) >> FLOAT32_TAIL_WIDTH) -
        FLOAT32_EXP_BASE + FLOAT16_EXP_BASE); // exponent
    if (exp > FLOAT16_EXP_MAX) { // inf
        result = FLOAT16_INF;
    } else if (exp <= 0) { // subnormal
        result = static_cast<uint16_t>(((bit32 & FLOAT32_TAIL_MASK) | (1 << FLOAT32_TAIL_WIDTH)) >>
            (static_cast<uint32_t>(FLOAT32_FLOAT16_TAIL_OFFSET + (1 - exp))));
    } else { // normal
        result = static_cast<uint16_t>(((bit32 & FLOAT32_TAIL_MASK) >> FLOAT32_FLOAT16_TAIL_OFFSET) |
            static_cast<uint32_t>(static_cast<uint32_t>(exp) << FLOAT16_TAIL_WIDTH));
    }
    if ((bit32 & FLOAT32_SIGN_MASK) != 0) {
        result |= FLOAT16_SIGN_MASK;
    }
    return result;
}

bool IsUnderFlowFloat(const std::string& literal)
{
    std::stringstream ss(literal);
    double value;
    // If the string is too large or too small to be represented by C++, the value obtained through ss is either a value
    // greater than 1 or less than 1.
    ss >> value;
    if (std::fabs(value) < 1.0) {
        return true;
    }
    return false;
}
} // namespace Cangjie::FloatFormat