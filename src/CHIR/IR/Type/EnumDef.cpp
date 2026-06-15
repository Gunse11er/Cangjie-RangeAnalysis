// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/IR/Type/EnumDef.h"

#include <sstream>
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/Utils/ToStringUtils.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

using namespace Cangjie::CHIR;

bool EnumDef::IsAllCtorsTrivial() const
{
    // if enum all ctor does not have params, it is a trivial enum
    for (auto& ctor : ctors) {
        if (ctor.funcType->GetParamTypes().size() != 0) {
            return false;
        }
    }
    return true;
}

std::string EnumDef::LocalVarToString() const
{
    std::stringstream ss;
    for (auto& ctor : ctors) {
        ss << AddNewLineOrNot(ctor.annoInfo.ToString(1));
        ss << IndentToString(1) <<
            "| " << ctor.name << TypeVecToString("(", ctor.funcType->GetParamTypes(), ")") << std::endl;
    }
    if (nonExhaustive) {
        ss << IndentToString(1) << "| ..." << std::endl;
    }
    return ss.str();
}

void EnumDef::AddCtor(EnumCtorInfo ctor)
{
    ctors.emplace_back(ctor);
}

std::vector<EnumCtorInfo> EnumDef::GetCtors() const
{
    return ctors;
}

void EnumDef::SetCtors(const std::vector<EnumCtorInfo>& items)
{
    ctors = items;
}

EnumCtorInfo EnumDef::GetCtor(size_t index) const
{
    CJC_ASSERT(ctors.size() > index);
    return ctors[index];
}

void EnumDef::SetType(CustomType& ty)
{
    CJC_ASSERT(ty.GetTypeKind() == Type::TypeKind::TYPE_ENUM);
    type = &ty;
}

EnumType* EnumDef::GetType() const
{
    return StaticCast<EnumType>(type);
}

bool EnumDef::IsExhaustive() const
{
    return !nonExhaustive;
}