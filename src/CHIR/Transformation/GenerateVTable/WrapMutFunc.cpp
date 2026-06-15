// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Transformation/GenerateVTable/WrapMutFunc.h"

#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/IR/Type/ExtendDef.h"
#include "cangjie/CHIR/Utils/UserDefinedType.h"
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/Utils/FileUtil.h"
#include "cangjie/Mangle/CHIRManglingUtils.h"

using namespace Cangjie::CHIR;
using namespace Cangjie;

namespace {
/**
 * DFS for a path from `cur` to `targetParent` (same ClassType object identity) in the inheritance graph.
 * On success, `outPath` is [target, ..., cur] with instantiated types.
 */
bool FindPathToTarget(ClassType& cur, ClassType& targetParent, CHIRBuilder& builder,
    const std::unordered_map<const GenericType*, Type*>& replaceTable, std::vector<ClassType*>& outPath,
    std::unordered_set<ClassType*>& onPath)
{
    if (&cur == &targetParent) {
        outPath.push_back(StaticCast<ClassType*>(ReplaceRawGenericArgType(cur, replaceTable, builder)));
        return true;
    }
    if (!onPath.insert(&cur).second) {
        return false;
    }

    auto unwindAndRecord =
        [&](ClassType& child, const std::unordered_map<const GenericType*, Type*>& childReplaceTable) {
        if (FindPathToTarget(child, targetParent, builder, childReplaceTable, outPath, onPath)) {
            outPath.push_back(StaticCast<ClassType*>(ReplaceRawGenericArgType(cur, replaceTable, builder)));
            return true;
        }
        return false;
    };

    for (auto ex : cur.GetCustomTypeDef()->GetExtends()) {
        // maybe we can meet `extend<T> A<B<T>> {}`, and `curType` is A<Int32>, then ignore this def,
        // so not need to check `res`
        auto [res, extendTable] = ex->GetExtendedType()->CalculateGenericTyMapping(cur);
        if (!res) {
            continue;
        }
        for (auto interface : ex->GetImplementedInterfaceTys()) {
            if (unwindAndRecord(*interface, extendTable)) {
                onPath.erase(&cur);
                return true;
            }
        }
    }
    for (auto interface : cur.GetImplementedInterfaceTys(&builder)) {
        if (unwindAndRecord(*interface, replaceTable)) {
            onPath.erase(&cur);
            return true;
        }
    }
    if (cur.GetSuperClassTy(&builder) != nullptr) {
        auto superClass = cur.GetSuperClassTy(&builder);
        if (unwindAndRecord(*superClass, replaceTable)) {
            onPath.erase(&cur);
            return true;
        }
    }
    onPath.erase(&cur);
    return false;
}

std::vector<ClassType*> GetTargetInheritanceList(CustomTypeDef& curDef, ClassType& targetParent, CHIRBuilder& builder)
{
    std::vector<ClassType*> path;
    std::unordered_set<ClassType*> onPath;
    std::unordered_map<const GenericType*, Type*> emptyTable;

    auto tryFrom = [&](ClassType& start) -> bool {
        path.clear();
        onPath.clear();
        return FindPathToTarget(start, targetParent, builder, emptyTable, path, onPath);
    };

    auto structTy = StaticCast<StructType*>(curDef.GetType());
    for (auto iface : structTy->GetImplementedInterfaceTys(&builder)) {
        if (tryFrom(*iface)) {
            return path;
        }
    }
    CJC_ASSERT(false);
    return path;
}

std::unordered_map<const GenericType*, Type*> CollectReplaceTable(
    CustomTypeDef& curDef, ClassType& targetParent, Function& rawFunc, CHIRBuilder& builder)
{
    auto inheritanceList = GetTargetInheritanceList(curDef, targetParent, builder);
    std::unordered_map<const GenericType*, Type*> replaceTable;
    bool found = false;
    auto targetParentDef = rawFunc.GetParentCustomTypeDef();
    for (auto parent : inheritanceList) {
        if (parent->GetCustomTypeDef() != targetParentDef) {
            continue;
        }
        std::tie(found, replaceTable) = parent->GetCustomTypeDef()->GetType()->CalculateGenericTyMapping(*parent);
        CJC_ASSERT(found);
        break;
    }
    return replaceTable;
}

std::vector<GenericType*> CreateWrapperFuncGenericParams(Function& rawFunc, const std::string& funcIdentifier,
    std::unordered_map<const GenericType*, Type*>& replaceTable, CHIRBuilder& builder)
{
    std::vector<GenericType*> wrapperGenericParams;
    auto rawFuncGenericParams = rawFunc.GetGenericTypeParams();
    wrapperGenericParams.reserve(rawFuncGenericParams.size());
    for (size_t i = 0; i < rawFuncGenericParams.size(); ++i) {
        auto srcIdentifier = "fT" + std::to_string(i);
        auto tyIdentifier = funcIdentifier + '_' + srcIdentifier;
        auto wrapperGenericParam = builder.GetType<GenericType>(tyIdentifier, srcIdentifier);
        wrapperGenericParams.emplace_back(wrapperGenericParam);
        replaceTable[rawFuncGenericParams[i]] = wrapperGenericParam;
    }
    for (size_t i = 0; i < wrapperGenericParams.size(); ++i) {
        std::vector<Type*> newUpperBounds;
        for (auto ty : rawFuncGenericParams[i]->GetUpperBounds()) {
            newUpperBounds.emplace_back(ReplaceRawGenericArgType(*ty, replaceTable, builder));
        }
        wrapperGenericParams[i]->SetUpperBounds(newUpperBounds);
    }
    return wrapperGenericParams;
}
} // namespace

void WrapMutFunc::CreateMutFuncWrapper(Function& rawFunc, CustomTypeDef& curDef, ClassType& srcClassTy)
{
    // create the wrapper func
    auto replaceTable = CollectReplaceTable(curDef, srcClassTy, rawFunc, builder);
    auto funcIdentifier = CHIRMangling::GenerateVirtualFuncMangleName(&rawFunc, curDef, &srcClassTy, false);
    auto wrapperGenericParams = CreateWrapperFuncGenericParams(rawFunc, funcIdentifier, replaceTable, builder);

    auto instFuncTy = StaticCast<FuncType*>(ReplaceRawGenericArgType(*rawFunc.GetFuncType(), replaceTable, builder));
    auto wrapperParamsTy = instFuncTy->GetParamTypes();
    auto parentDefType = curDef.GetType();
    wrapperParamsTy[0] = builder.GetType<RefType>(parentDefType);
    auto retTy = instFuncTy->GetReturnType();
    auto wrapperFuncTy = builder.GetType<FuncType>(wrapperParamsTy, retTy);

    auto pkgName = curDef.GetPackageName();
    auto func = builder.CreateFunction(wrapperFuncTy, funcIdentifier, "", "", pkgName, wrapperGenericParams);
    wrapperFuncs.emplace(funcIdentifier, func);

    func->Set<WrappedRawMethod>(&rawFunc);
    func->AppendAttributeInfo(rawFunc.GetAttributeInfo());
    func->DisableAttr(Attribute::VIRTUAL);
    func->EnableAttr(Attribute::NO_REFLECT_INFO);
    curDef.AddMethod(func);

    if (curDef.TestAttr(Attribute::IMPORTED)) {
        func->EnableAttr(Attribute::IMPORTED);
        return;
    }

    func->DisableAttr(Attribute::IMPORTED);
    // create the func body
    BlockGroup* body = builder.CreateBlockGroup(*func);
    func->InitBody(*body);

    std::vector<Value*> args;
    for (auto paramTy : wrapperParamsTy) {
        args.emplace_back(builder.CreateParameter(paramTy, INVALID_LOCATION, *func));
    }

    auto entry = builder.CreateBlock(body);
    body->SetEntryBlock(entry);
    auto ret =
        Cangjie::CHIR::CreateAndAppendExpression<Allocate>(builder, builder.GetType<RefType>(retTy), retTy, entry);
    func->SetReturnValue(*ret->GetResult());

    auto rawFuncFirstArgType = rawFunc.GetFuncType()->GetParamTypes()[0]->StripAllRefs();
    auto firstArgType = GetInstSubType(*rawFuncFirstArgType, srcClassTy, builder);
    if (!firstArgType->IsValueType() || rawFunc.TestAttr(Attribute::MUT)) {
        firstArgType = builder.GetType<RefType>(firstArgType);
    }
    args[0] = Cangjie::CHIR::TypeCastOrBoxIfNeeded(*args[0], *firstArgType, builder, *entry, INVALID_LOCATION);

    std::vector<Type*> instArgTypes;
    instArgTypes.reserve(wrapperGenericParams.size());
    for (auto wrapperGenericParam : wrapperGenericParams) {
        instArgTypes.emplace_back(wrapperGenericParam);
    }
    auto apply = Cangjie::CHIR::CreateAndAppendExpression<Apply>(builder, retTy, &rawFunc, FuncCallContext{
        .args = args,
        .instTypeArgs = instArgTypes,
        .thisType = curDef.GetType()}, entry);
    Cangjie::CHIR::CreateAndAppendExpression<Store>(
        builder, builder.GetUnitTy(), apply->GetResult(), func->GetReturnValue(), entry);

    auto tempThis =
        Cangjie::CHIR::TypeCastOrBoxIfNeeded(*args[0], *wrapperParamsTy[0], builder, *entry, INVALID_LOCATION);
    auto load = Cangjie::CHIR::CreateAndAppendExpression<Load>(builder, parentDefType, tempThis, entry)->GetResult();
    auto structMemberTypes = StaticCast<StructType*>(parentDefType)->GetInstantiatedMemberTys(builder);

    for (size_t i = 0; i < structMemberTypes.size(); ++i) {
        auto path = std::vector<uint64_t>{i};
        auto field = Cangjie::CHIR::CreateAndAppendExpression<Field>(builder, structMemberTypes[i], load, path, entry)
            ->GetResult();
        Cangjie::CHIR::CreateAndAppendExpression<StoreElementRef>(
            builder, builder.GetUnitTy(), field, func->GetParam(0), path, entry);
    }

    entry->AppendExpression(builder.CreateTerminator<Exit>(entry));
}

void WrapMutFunc::Run(CustomTypeDef& customTypeDef)
{
    Type* structTy = nullptr;
    if (customTypeDef.GetCustomKind() == CustomDefKind::TYPE_EXTEND &&
        StaticCast<ExtendDef>(customTypeDef).GetExtendedType()->IsStruct()) {
        structTy = StaticCast<ExtendDef>(customTypeDef).GetExtendedType();
    } else if (customTypeDef.GetCustomKind() == CustomDefKind::TYPE_STRUCT &&
        StaticCast<StructDef>(customTypeDef).GetImplementedInterfacesNum() > 0) {
        structTy = customTypeDef.GetType();
    }
    if (!structTy) {
        return;
    }
    for (auto& vtableIt : customTypeDef.GetDefVTable().GetTypeVTables()) {
        for (auto& methodInfo : vtableIt.GetVirtualMethods()) {
            if (methodInfo.GetVirtualMethod()->IsPureAbstract()) {
                continue;
            }
            auto rawFunc = methodInfo.GetVirtualMethod();
            while (auto base = rawFunc->Get<WrappedRawMethod>()) {
                rawFunc = base;
            }
            if (!rawFunc->TestAttr(Attribute::MUT) || rawFunc->GetParentCustomTypeDef() == &customTypeDef) {
                continue;
            }
            if (auto ex = DynamicCast<ExtendDef*>(&customTypeDef);
                ex && ex->GetExtendedCustomTypeDef() == rawFunc->GetParentCustomTypeDef()) {
                continue;
            }
            CreateMutFuncWrapper(*rawFunc, customTypeDef, *vtableIt.GetSrcParentType());
        }
    }
}

WrapMutFunc::WrapMutFunc(CHIRBuilder& b) : builder(b)
{
}

std::unordered_map<std::string, Function*>&& WrapMutFunc::GetWrappers()
{
    return std::move(wrapperFuncs);
}
