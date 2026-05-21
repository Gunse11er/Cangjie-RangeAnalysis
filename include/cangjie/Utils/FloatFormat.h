// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_UTILS_FLOATFORMAT_H
#define CANGJIE_UTILS_FLOATFORMAT_H

#include <cstdint>
#include <string>

namespace Cangjie::FloatFormat {
uint16_t Float32ToFloat16(float value);
bool IsUnderFlowFloat(const std::string& literal);
} // namespace Cangjie::FloatFormat

#endif // CANGJIE_UTILS_FLOATFORMAT_H