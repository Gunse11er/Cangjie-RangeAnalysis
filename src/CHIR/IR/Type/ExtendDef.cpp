// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/IR/Type/ExtendDef.h"

#include <sstream>
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/Utils/ToStringUtils.h"
#include "cangjie/Utils/CastingTemplate.h"

using namespace Cangjie;
using namespace Cangjie::CHIR;

ExtendDef::ExtendDef(
    const std::string& identifier, const std::string& pkgName, std::vector<GenericType*> genericParams)
    : CustomTypeDef("", identifier, pkgName, CustomDefKind::TYPE_EXTEND), genericParams(genericParams)
{
}

CustomTypeDef* ExtendDef::GetExtendedCustomTypeDef() const
{
    if (auto customTy = DynamicCast<const CustomType*>(extendedType); customTy) {
        return customTy->GetCustomTypeDef();
    }
    return nullptr;
}

std::string ExtendDef::CustomTypeDefTitleToString() const
{
    std::string extendedTyStr;
    CJC_NULLPTR_CHECK(extendedType);
    if (auto customTy = DynamicCast<const CustomType*>(extendedType)) {
        extendedTyStr = customTy->GetCustomTypeDef()->GetIdentifier() + GenericInsArgsToString(*customTy);
    } else {
        extendedTyStr = extendedType->ToString();
    }
    return "extend" + TypeVecToString("<", genericParams, ">") + " " + extendedTyStr;
}

std::string ExtendDef::AddExtraComment() const
{
    std::vector<std::string> result;
    if (!identifier.empty()) {
        result.emplace_back("id: " + identifier);
    }
    if (auto gStr = GetGenericTypeConstaintsStr(genericParams); !gStr.empty()) {
        result.emplace_back("genericConstrains: " + gStr);
    }
    return StringJoin(result, ", ");
}

void ExtendDef::RemoveParent(ClassType& parent)
{
    implementedInterfaceTys.erase(
        std::remove(implementedInterfaceTys.begin(), implementedInterfaceTys.end(), &parent),
        implementedInterfaceTys.end());
}

Type* ExtendDef::GetExtendedType() const
{
    CJC_NULLPTR_CHECK(extendedType);
    return extendedType;
}

Type* ExtendDef::GetType() const
{
    return GetExtendedType();
}

void ExtendDef::SetExtendedType(Type& ty)
{
    extendedType = &ty;
}

void ExtendDef::SetType(CustomType& ty)
{
    (void)ty;
    CJC_ABORT(); // extend decl doesn't have type
}

std::vector<GenericType*> ExtendDef::GetGenericTypeParams() const
{
    return genericParams;
}