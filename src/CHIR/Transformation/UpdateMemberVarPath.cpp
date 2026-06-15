// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Transformation/UpdateMemberVarPath.h"

#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/CHIR/Utils/Visitor/Visitor.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "cangjie/Utils/ProfileRecorder.h"

using namespace Cangjie::CHIR;

UpdateMemberVarPath::UpdateMemberVarPath(Package& pkg, CHIRBuilder& builder) : pkg(pkg), builder(builder)
{
}

void UpdateMemberVarPath::Run()
{
    Utils::ProfileRecorder recorder("Canonicalization", "UpdateMemberVarPath");
    auto preVisit = [this](Expression& e) {
        if (auto get = DynamicCast<GetElementByName*>(&e)) {
            UpdateToGetElementRef(*get);
        } else if (auto store = DynamicCast<StoreElementByName*>(&e)) {
            UpdateToStoreElementRef(*store);
        } else if (auto field = DynamicCast<FieldByName*>(&e)) {
            UpdateToField(*field);
        }
        return VisitResult::CONTINUE;
    };
    for (auto func : pkg.GetGlobalFuncsWithBody()) {
        Visitor::Visit(*func, preVisit);
    }
}

std::pair<Type*, uint64_t> UpdateMemberVarPath::GetIndexByName(
    const CustomType& baseType, const std::string& name)
{
    Type* memberVarInstType = nullptr;
    std::unordered_map<const GenericType*, Type*> instMap;
    baseType.GetInstMap(instMap, builder);
    auto allMemberVars = baseType.GetCustomTypeDef()->GetAllInstanceVars();
    uint64_t index = allMemberVars.size();
    // be sure to use reverse order traversal
    // because sub class and parent class may have private member vars with same name
    for (auto it = allMemberVars.crbegin(); it != allMemberVars.crend(); ++it) {
        --index;
        if (it->name == name) {
            memberVarInstType = ReplaceRawGenericArgType(*it->type, instMap, builder);
            break;
        }
    }
    CJC_NULLPTR_CHECK(memberVarInstType);
    return {memberVarInstType, index};
}

std::vector<uint64_t> UpdateMemberVarPath::ChangeNameToPath(
    CustomType& rootType, const std::vector<std::string>& names)
{
    std::vector<uint64_t> path;
    CustomType* baseType = &rootType;
    for (const auto& name : names) {
        CJC_NULLPTR_CHECK(baseType);
        auto res = GetIndexByName(*baseType, name);
        path.emplace_back(res.second);
        baseType = DynamicCast<CustomType*>(res.first);
    }
    return path;
}

void UpdateMemberVarPath::UpdateToGetElementRef(GetElementByName& rawExpr)
{
    auto locationVal = rawExpr.GetLocation();
    auto locationType = StaticCast<CustomType*>(locationVal->GetType()->StripAllRefs());
    auto path = ChangeNameToPath(*locationType, rawExpr.GetNames());
    auto loc = rawExpr.GetDebugLocation();
    auto retType = rawExpr.GetResult()->GetType();
    auto parentBlock = rawExpr.GetParentBlock();
    auto newExpr = builder.CreateExpression<GetElementRef>(loc, retType, locationVal, path, parentBlock);
    newExpr->CopyBaseInfoFrom(rawExpr);
    newExpr->GetResult()->CopyBaseInfoFrom(*rawExpr.GetResult());
    newExpr->GetResult()->AppendAttributeInfo(rawExpr.GetResult()->GetAttributeInfo());
    rawExpr.ReplaceWith(*newExpr);
}

void UpdateMemberVarPath::UpdateToStoreElementRef(StoreElementByName& rawExpr)
{
    auto locationVal = rawExpr.GetLocation();
    auto locationType = StaticCast<CustomType*>(locationVal->GetType()->StripAllRefs());
    auto path = ChangeNameToPath(*locationType, rawExpr.GetNames());
    auto value = rawExpr.GetValue();
    auto loc = rawExpr.GetDebugLocation();
    auto retType = rawExpr.GetResult()->GetType();
    auto parentBlock = rawExpr.GetParentBlock();
    auto newExpr = builder.CreateExpression<StoreElementRef>(loc, retType, value, locationVal, path, parentBlock);
    newExpr->CopyBaseInfoFrom(rawExpr);
    newExpr->GetResult()->CopyBaseInfoFrom(*rawExpr.GetResult());
    newExpr->GetResult()->AppendAttributeInfo(rawExpr.GetResult()->GetAttributeInfo());
    rawExpr.ReplaceWith(*newExpr);
}

void UpdateMemberVarPath::UpdateToField(FieldByName& rawExpr)
{
    auto locationVal = rawExpr.GetBase();
    auto locationType = StaticCast<CustomType*>(locationVal->GetType()->StripAllRefs());
    auto path = ChangeNameToPath(*locationType, rawExpr.GetNames());
    auto loc = rawExpr.GetDebugLocation();
    auto retType = rawExpr.GetResult()->GetType();
    auto parentBlock = rawExpr.GetParentBlock();
    auto newExpr = builder.CreateExpression<Field>(loc, retType, locationVal, path, parentBlock);
    newExpr->CopyBaseInfoFrom(rawExpr);
    newExpr->GetResult()->CopyBaseInfoFrom(*rawExpr.GetResult());
    newExpr->GetResult()->AppendAttributeInfo(rawExpr.GetResult()->GetAttributeInfo());
    rawExpr.ReplaceWith(*newExpr);
}
