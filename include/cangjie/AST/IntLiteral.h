// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares int literal apis.
 */

#ifndef CANGJIE_INTLITERAL_H
#define CANGJIE_INTLITERAL_H

#include <string>

#include "cangjie/AST/Types.h"

namespace Cangjie::AST {
class IntLiteral {
public:
    IntLiteral() = default;
    IntLiteral(const std::string& stringVal, const TypeKind kind) : type(kind)
    {
        InitIntLiteral(stringVal, kind);
    }

    IntLiteral(const uint64_t val, const TypeKind kind, bool overflow, bool max = false)
        : int64Val(static_cast<int64_t>(val)), uint64Val(val), outOfRange(overflow), outOfMax(max), type(kind)
    {
        // Calc wrapping value and saturating value.
        CalcWrappingAndSaturatingVal();
    }

    IntLiteral(const int64_t val, const TypeKind kind, bool overflow, bool max = false)
        : int64Val(val), uint64Val(static_cast<uint64_t>(val)), outOfRange(overflow), outOfMax(max), type(kind)
    {
        sign = (IsUnsigned() || int64Val >= 0) ? 1 : -1;
        CalcWrappingAndSaturatingVal();
    }

    ~IntLiteral() = default;

    void Assign(const IntLiteral& other)
    {
        sign = other.sign;
        int64Val = other.int64Val;
        uint64Val = other.uint64Val;
        outOfRange = other.outOfRange;
        type = other.type;
    }

    int Sign() const
    {
        return sign;
    }
    void SetSign(int input)
    {
        sign = input;
    }

    int64_t Int64() const
    {
        return int64Val;
    }
    void SetInt64(int64_t input)
    {
        int64Val = input;
        uint64Val = static_cast<uint64_t>(input);
    }

    uint64_t Uint64() const
    {
        return uint64Val;
    }
    void SetUint64(uint64_t input)
    {
        uint64Val = input;
        int64Val = static_cast<int64_t>(input);
    }

    void CalcWrappingAndSaturatingVal();
    void SetWrappingValue();
    void SetSaturatingValue();
    std::string GetValue() const;

    bool IsOutOfRange() const
    {
        return outOfRange;
    }
    bool IsNegativeNum() const
    {
        return sign == -1 && uint64Val != 0;
    }
    void InitIntLiteral(const std::string& stringVal, const TypeKind kind);

    bool GreaterThanOrEqualBitLen(const TypeKind kind) const;

    void SetOutOfRange(Ptr<const Ty> ty);

private:
    bool CheckOverflow();
    bool IsUnsigned() const
    {
        return type >= TypeKind::TYPE_UINT8 && type <= TypeKind::TYPE_UINT64;
    }
    uint64_t QuickPow(const uint64_t inBase, const uint64_t inExp, const uint64_t maxVal, bool& overflow) const;
    uint64_t GetAbsValue() const;
    int sign{1};
    int64_t int64Val{0};
    uint64_t uint64Val{0};
    int64_t sint64Val{0};   // saturating int value if outOfRange is true
    uint64_t suint64Val{0}; // saturating uint value if outOfRange is true
    int64_t wint64Val{0};   // wrapping int value if outOfRange is true
    uint64_t wuint64Val{0}; // wrapping uint value if outOfRange is true
    bool outOfRange{false};
    // outOfMax is useful only if outOfRange is true
    bool outOfMax{false}; // outOfMax=true means Maximum overflow, otherwise Minimum overflow
    TypeKind type{TypeKind::TYPE_IDEAL_INT};
};
} // namespace Cangjie::AST

#endif // CANGJIE_INTLITERAL_H
