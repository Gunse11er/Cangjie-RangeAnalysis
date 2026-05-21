// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/AST/Identifier.h"

#include <iostream>

#include "cangjie/Utils/ConstantsUtils.h"

namespace Cangjie {
Identifier::Identifier(std::string s, const Position& begin, const Position& end)
{
    SetValue(std::move(s));
    SetPos(begin, end);
}

Identifier& Identifier::operator=(std::string_view identifier)
{
    std::string s{identifier};
    SetValue(std::move(s));
    return *this;
}
Identifier& Identifier::operator=(const std::string& identifier)
{
    std::string s{identifier};
    SetValue(std::move(s));
    return *this;
}
Identifier& Identifier::operator=(std::string&& identifier)
{
    SetValue(std::move(identifier));
    return *this;
}

bool Identifier::Valid() const
{
    return v != INVALID_IDENTIFIER;
}

std::ostream& operator<<(std::ostream& out, const Identifier& identifier)
{
    return out << identifier.Val() << ", begin = " << identifier.Begin() << ", end = " << identifier.End();
}

void Identifier::SetValue(std::string&& s)
{
    std::swap(v, s);
}
} // namespace Cangjie
