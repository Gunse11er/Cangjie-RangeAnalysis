// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements arithmetic operator.
 */

#include "cangjie/CHIR/Analysis/Arithmetic.h"

namespace Cangjie::CHIR {
int64_t SignExtend64(uint64_t val, unsigned srcWidth)
{
    CJC_ASSERT(srcWidth > 0 && srcWidth <= B64);
    return static_cast<int64_t>(val << (B64 - srcWidth)) >> (B64 - srcWidth);
}
}  // namespace Cangjie::CHIR
