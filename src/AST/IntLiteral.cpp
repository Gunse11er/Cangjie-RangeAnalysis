// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements int literal apis.
 */

#include "cangjie/AST/IntLiteral.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "cangjie/AST/ASTCasting.h"
#include "cangjie/Utils/StdUtils.h"

using namespace Cangjie;
using namespace AST;

namespace {
const std::map<TypeKind, uint64_t> INTEGER_TO_MAX_VALUE{
    {TypeKind::TYPE_INT8, std::numeric_limits<int8_t>::max()},
    {TypeKind::TYPE_INT16, std::numeric_limits<int16_t>::max()},
    {TypeKind::TYPE_INT32, std::numeric_limits<int32_t>::max()},
    {TypeKind::TYPE_INT64, std::numeric_limits<int64_t>::max()},
    {TypeKind::TYPE_UINT8, std::numeric_limits<uint8_t>::max()},
    {TypeKind::TYPE_UINT16, std::numeric_limits<uint16_t>::max()},
    {TypeKind::TYPE_UINT32, std::numeric_limits<uint32_t>::max()},
    {TypeKind::TYPE_UINT64, std::numeric_limits<uint64_t>::max()},
    {TypeKind::TYPE_IDEAL_INT, std::numeric_limits<uint64_t>::max()},
};

const std::map<TypeKind, int64_t> INTEGER_TO_MIN_VALUE{
    {TypeKind::TYPE_INT8, std::numeric_limits<int8_t>::min()},
    {TypeKind::TYPE_INT16, std::numeric_limits<int16_t>::min()},
    {TypeKind::TYPE_INT32, std::numeric_limits<int32_t>::min()},
    {TypeKind::TYPE_INT64, std::numeric_limits<int64_t>::min()},
    {TypeKind::TYPE_UINT8, std::numeric_limits<uint8_t>::min()},
    {TypeKind::TYPE_UINT16, std::numeric_limits<uint16_t>::min()},
    {TypeKind::TYPE_UINT32, std::numeric_limits<uint32_t>::min()},
    {TypeKind::TYPE_UINT64, std::numeric_limits<uint64_t>::min()},
    {TypeKind::TYPE_IDEAL_INT, std::numeric_limits<int64_t>::min()},
};

const std::map<TypeKind, uint64_t> INTEGER_TO_BIT_LEN{
    {TypeKind::TYPE_INT8, 8},
    {TypeKind::TYPE_INT16, 16},
    {TypeKind::TYPE_INT32, 32},
    {TypeKind::TYPE_INT64, 64},
    {TypeKind::TYPE_UINT8, 8},
    {TypeKind::TYPE_UINT16, 16},
    {TypeKind::TYPE_UINT32, 32},
    {TypeKind::TYPE_UINT64, 64},
    {TypeKind::TYPE_IDEAL_INT, 64},
};
} // namespace

static const size_t I64_WIDTH = 64;
static const size_t UI64_WIDTH = 64;

bool IntLiteral::GreaterThanOrEqualBitLen(const TypeKind kind) const
{
    auto iter = INTEGER_TO_BIT_LEN.find(kind);
    if (iter == INTEGER_TO_BIT_LEN.end()) {
        return false;
    }
    return uint64Val >= iter->second;
}

static int EscapeCharacterToInt(char c)
{
    switch (c) {
        case 't':
            return static_cast<int>('\t');
        case 'b':
            return static_cast<int>('\b');
        case 'r':
            return static_cast<int>('\r');
        case 'n':
            return static_cast<int>('\n');
        case '\'':
            return static_cast<int>('\'');
        case '"':
            return static_cast<int>('\"');
        case '\\':
            return static_cast<int>('\\');
        case 'f':
            return static_cast<int>('\f');
        case 'v':
            return static_cast<int>('\v');
        case '0':
            return static_cast<int>('\0');
        default:
            return -1;
    }
}

// return -1 if encounter errors
// parse byte such as b'x', b'\n', b'\u{ff}'
static int ParseByteIntLitString(const std::string& val)
{
    size_t startQuote = val.find_first_of('\'', 0);
    size_t endQuote = val.find_last_of('\'', val.size() - 1);
    // 2 is minimum value. It must start with 'b' and '\'' since this is a lexer rule.
    if (endQuote - startQuote < 2) {
        return -1;
    }
    std::string lit = val.substr(startQuote + 1, (endQuote - startQuote) - 1);
    constexpr size_t esCharLen = 2; // 2 is the length of escape characters.
    if (lit.size() == 1) {
        return static_cast<int>(lit[0]);
    } else if (lit.size() == esCharLen && lit[0] == '\\') {
        return EscapeCharacterToInt(lit[1]);
    } else if (lit.size() > esCharLen && lit[1] == 'u') {
        size_t lCurlPos = val.find_first_of('{', 0);
        size_t rCurlPos = val.find_first_of('}', 0);
        if (lCurlPos == std::string::npos || rCurlPos == std::string::npos || rCurlPos - lCurlPos <= 1) {
            return -1;
        }
        std::string digits = val.substr(lCurlPos + 1, (rCurlPos - lCurlPos) - 1);
        const int hexBase = 16;
        return Stoi(digits, hexBase).value_or(-1);
    }
    return -1;
}

static std::pair<int, std::string> GetBaseAndPureStringValue(const std::string& stringVal)
{
    const int baseLen = 2;
    const int binBase = 2;
    const int octBase = 8;
    const int hexBase = 16;
    int base = 10;
    bool isNegNum = false;
    std::string prefix;
    std::string stringValue = stringVal;

    if (stringValue.at(0) == '-') {
        stringValue.erase(0, 1);
        isNegNum = true;
    }

    if (stringValue.size() > baseLen) {
        prefix = stringValue.substr(0, baseLen);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
    }
    // Erase underline. like 1_2_3 ==> 123
    stringValue.erase(std::remove(stringValue.begin(), stringValue.end(), '_'), stringValue.end());
    // Get base.
    if (prefix == "0b") {
        stringValue = stringValue.erase(0, baseLen);
        base = binBase;
    } else if (prefix == "0o") {
        stringValue = stringValue.erase(0, baseLen);
        base = octBase;
    } else if (prefix == "0x") {
        stringValue = stringValue.erase(0, baseLen);
        base = hexBase;
    }
    if (isNegNum) {
        stringValue = "-" + stringValue;
    }
    return std::make_pair(base, stringValue);
}

void IntLiteral::InitIntLiteral(const std::string& stringVal, const TypeKind kind)
{
    if (stringVal.empty()) {
        return;
    }
    type = kind;

    if (stringVal.at(0) == '-') {
        sign = -1;
    }

    auto [base, stringValue] = GetBaseAndPureStringValue(stringVal);

    bool outOfUintRange = false;
    bool outOfIntRange = false;

    if (stringVal[0] == 'b') {
        int64Val = ParseByteIntLitString(stringVal);
        if (int64Val == -1) {
            int64Val = 0;
            outOfIntRange = true;
            outOfUintRange = true;
        }
        uint64Val = static_cast<uint64_t>(int64Val);
    } else {
        if (auto ul = Stoull(stringValue, base)) {
            uint64Val = *ul;
        } else {
            uint64Val = 0;
            outOfUintRange = true;
        }

        if (auto il = Stoll(stringValue, base)) {
            int64Val = *il;
        } else {
            int64Val = 0;
            outOfIntRange = true;
        }
    }
    outOfRange = sign == -1 ? outOfIntRange : outOfUintRange;
}

static TypeKind GetRealTypeKindOfNative(Ptr<const Ty> ty)
{
    CJC_ASSERT(ty);
    TypeKind k = ty->kind;
    if (k == TypeKind::TYPE_INT_NATIVE) {
        k = StaticCast<const PrimitiveTy*>(ty)->bitness == I64_WIDTH ? TypeKind::TYPE_INT64 : TypeKind::TYPE_INT32;
    } else if (k == TypeKind::TYPE_UINT_NATIVE) {
        k = StaticCast<const PrimitiveTy*>(ty)->bitness == UI64_WIDTH ? TypeKind::TYPE_UINT64 : TypeKind::TYPE_UINT32;
    }
    return k;
}

void IntLiteral::SetOutOfRange(Ptr<const Ty> ty)
{
    type = GetRealTypeKindOfNative(ty);
    outOfRange = outOfRange || CheckOverflow(); // check overflow if convert not out of range
}

bool IntLiteral::CheckOverflow()
{
    if (INTEGER_TO_MAX_VALUE.find(type) == INTEGER_TO_MAX_VALUE.end()) {
        return false;
    }
    if (sign == 1) {
        return uint64Val > INTEGER_TO_MAX_VALUE.at(type);
    } else {
        return int64Val < INTEGER_TO_MIN_VALUE.at(type);
    }
}

namespace {
uint64_t CalcWrappingValue(uint64_t value, TypeKind type)
{
    if (type == TypeKind::TYPE_UINT8) {
        return static_cast<uint64_t>(static_cast<uint8_t>(value));
    }
    if (type == TypeKind::TYPE_UINT16) {
        return static_cast<uint64_t>(static_cast<uint16_t>(value));
    }
    if (type == TypeKind::TYPE_UINT32) {
        return static_cast<uint64_t>(static_cast<uint32_t>(value));
    }
    return value;
}

int64_t CalcWrappingValue(int64_t value, TypeKind type)
{
    if (type == TypeKind::TYPE_INT8) {
        return static_cast<int64_t>(static_cast<int8_t>(value));
    }
    if (type == TypeKind::TYPE_INT16) {
        return static_cast<int64_t>(static_cast<int16_t>(value));
    }
    if (type == TypeKind::TYPE_INT32) {
        return static_cast<int64_t>(static_cast<int32_t>(value));
    }
    return value;
}
} // namespace

void IntLiteral::CalcWrappingAndSaturatingVal()
{
    if (!outOfRange) {
        return;
    }
    if (INTEGER_TO_MAX_VALUE.find(type) == INTEGER_TO_MAX_VALUE.end()) {
        return;
    }
    if (IsUnsigned()) {
        wuint64Val = CalcWrappingValue(uint64Val, type);
        if (outOfMax) {
            suint64Val = INTEGER_TO_MAX_VALUE.at(type);
        } else {
            suint64Val = static_cast<uint64_t>(INTEGER_TO_MIN_VALUE.at(type));
        }
    } else {
        wint64Val = CalcWrappingValue(int64Val, type);
        if (outOfMax) {
            sint64Val = static_cast<int64_t>(INTEGER_TO_MAX_VALUE.at(type));
        } else {
            sint64Val = INTEGER_TO_MIN_VALUE.at(type);
        }
    }
}

void IntLiteral::SetWrappingValue()
{
    if (!outOfRange) {
        return;
    }
    if (IsUnsigned()) {
        SetUint64(wuint64Val);
        return;
    }
    SetInt64(wint64Val);
}

void IntLiteral::SetSaturatingValue()
{
    if (!outOfRange) {
        return;
    }
    if (IsUnsigned()) {
        SetUint64(suint64Val);
        return;
    }
    SetInt64(sint64Val);
}

std::string IntLiteral::GetValue() const
{
    if (IsUnsigned()) {
        return std::to_string(uint64Val);
    }
    return std::to_string(int64Val);
}

uint64_t IntLiteral::QuickPow(const uint64_t inBase, const uint64_t inExp, const uint64_t maxVal, bool& overflow) const
{
    uint64_t res = 1;
    uint64_t base = inBase;
    uint64_t exp = inExp;
    bool ov = false;
    while (exp != 0 && !overflow) {
        if ((exp & 1) != 0) {
            if (ov || (base != 0 && (maxVal / base < res))) {
                overflow = true;
            }
            res *= base;
        }
        if (!ov && (base != 0 && (maxVal / base < base))) {
            ov = true;
        }
        base *= base;
        exp >>= 1;
    }
    return res;
}

uint64_t IntLiteral::GetAbsValue() const
{
    /* The min value of int64_t cast to uint64_t is exactly its absolute value. */
    if (int64Val == std::numeric_limits<int64_t>::min()) {
        return static_cast<uint64_t>(int64Val);
    }
    return sign == 1 ? uint64Val : static_cast<uint64_t>(-int64Val);
}
