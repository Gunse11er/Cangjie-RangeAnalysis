// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Analysis/BoolDomain.h"

namespace Cangjie::CHIR {
constexpr unsigned N{0};
constexpr unsigned F{1};
constexpr unsigned T{2};
constexpr unsigned A{3};
constexpr unsigned OP_TABLE_LEN{4};

BoolDomain::BoolDomain(const BoolDomain& other) : v{other.v}
{
}

BoolDomain::BoolDomain(BoolDomain&& other) : v{other.v}
{
}

BoolDomain& BoolDomain::operator=(const BoolDomain& other)
{
    if (&other != this) {
        v = other.v;
    }
    return *this;
}
BoolDomain& BoolDomain::operator=(BoolDomain&& other)
{
    if (&other != this) {
        v = other.v;
    }
    return *this;
}

BoolDomain::~BoolDomain()
{
}

BoolDomain::BoolDomain(unsigned v) : v{v}
{
}

BoolDomain BoolDomain::Bottom()
{
    return BoolDomain{N};
}

BoolDomain BoolDomain::False()
{
    return BoolDomain{F};
}

BoolDomain BoolDomain::True()
{
    return BoolDomain{T};
}

BoolDomain BoolDomain::Top()
{
    return BoolDomain{A};
}

bool BoolDomain::IsTrue() const
{
    return v == T;
}

bool BoolDomain::IsFalse() const
{
    return v == F;
}

bool BoolDomain::IsTop() const
{
    return v == A;
}

bool BoolDomain::IsBottom() const
{
    return v == N;
}

bool BoolDomain::IsNonTrivial() const
{
    return !IsTop();
}

bool BoolDomain::IsSingleValue() const
{
    return v == T || v == F;
}

bool BoolDomain::GetSingleValue() const
{
    return v == T;
}

BoolDomain BoolDomain::FromBool(bool v)
{
    return v ? BoolDomain{T} : BoolDomain{F};
}

BoolDomain LogicalAnd(const BoolDomain& a, const BoolDomain& b)
{
    static constexpr unsigned logicalAndTable[OP_TABLE_LEN][OP_TABLE_LEN]{
        {N, N, N, N},
        {N, F, F, F},
        {N, F, T, A},
        {N, F, A, A},
    };
    return BoolDomain{logicalAndTable[a.v][b.v]};
}

BoolDomain LogicalOr(const BoolDomain& a, const BoolDomain& b)
{
    static constexpr unsigned logicalOrTable[OP_TABLE_LEN][OP_TABLE_LEN]{
        {N, N, N, N},
        {N, F, T, A},
        {N, T, T, T},
        {N, A, T, A},
    };
    return BoolDomain{logicalOrTable[a.v][b.v]};
}

BoolDomain operator&(const BoolDomain& a, const BoolDomain& b)
{
    return BoolDomain{a.v & b.v};
}

BoolDomain operator|(const BoolDomain& a, const BoolDomain& b)
{
    return BoolDomain{a.v | b.v};
}

BoolDomain operator!(const BoolDomain& v)
{
    static constexpr unsigned logicalNotTable[]{N, T, F, A};
    return BoolDomain{logicalNotTable[v.v]};
}

std::ostream& operator<<(std::ostream& out, const BoolDomain& v)
{
    static const std::string B[OP_TABLE_LEN]{"<>", "false", "true", "<t,f>"};
    return out << B[v.v];
}

BoolDomain BoolDomain::Union(const BoolDomain& a, const BoolDomain& b)
{
    return a | b;
}

bool BoolDomain::IsSame(const BoolDomain& domain) const
{
    return v == domain.v;
}

} // namespace Cangjie::CHIR
