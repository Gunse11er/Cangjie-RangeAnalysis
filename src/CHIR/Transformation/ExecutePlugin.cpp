// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Transformation/ExecutePlugin.h"

#include "cangjie/CHIR/Serializer/CHIRDeserializer.h"
#include "cangjie/CHIR/Serializer/CHIRSerializer.h"
#include "cangjie/Macro/InvokeUtil.h"

using namespace Cangjie;
using namespace Cangjie::CHIR;

ExecutePlugin::ExecutePlugin(CHIRBuilder& builder) : builder(builder) {}

ExecutePlugin::~ExecutePlugin()
{
    if (handle != nullptr) {
        InvokeRuntime::CloseSymbolTable(handle);
    }
}

bool ExecutePlugin::SerializePackage(const Package& package)
{
    serializedData = CHIRSerializer::Serialize(package);
    if (serializedData.data() == nullptr || serializedData.size() == 0) {
        return false;
    }
    return true;
}

bool ExecutePlugin::Execute(const std::string& pluginPath)
{
    handle = InvokeRuntime::OpenSymbolTableSafely(pluginPath);
    if (handle == nullptr) {
        return false;
    }
    void* fPtr = InvokeRuntime::GetMethod(handle, "executeCHIRPlugins");
    if (fPtr == nullptr) {
        return false;
    }
    pluginResult = reinterpret_cast<PluginResult (*)(uint8_t*, int64_t)>(fPtr)(
        serializedData.data(), static_cast<int64_t>(serializedData.size()));
    return pluginResult.success;
}

Package* ExecutePlugin::DeserializePluginResult(
    std::unordered_set<Function*>& srcCodeImportedFuncs,
    std::unordered_set<GlobalVar*>& srcCodeImportedVars,
    std::vector<Function*>& initFuncsForConstVar,
    std::unordered_map<std::string, Function*>& implicitFuncs,
    std::unordered_map<Block*, Terminator*>& maybeUnreachable)
{
    // 1. convert CHIR pointers to strings
    CHIRPtrToString(srcCodeImportedFuncs, srcCodeImportedVars, initFuncsForConstVar, implicitFuncs, maybeUnreachable);

    // 2. reset CHIR context
    builder.MergeAllocatedInstance();
    builder.GetChirContext().FreeWholePackage();
    builder.GetChirContext().Init();

    // 3. deserialize plugin result to cpp package
#ifndef CANGJIE_ENABLE_GCOV
    try {
#endif
        if (!CHIRDeserializer::Deserialize(pluginResult.data, pluginResult.size, builder)) {
            return nullptr;
        }
#ifndef CANGJIE_ENABLE_GCOV
    } catch (...) {
        return nullptr;
    }
#endif

    // 4. convert strings to CHIR pointers
    StringToCHIRPtr(srcCodeImportedFuncs, srcCodeImportedVars, initFuncsForConstVar, implicitFuncs, maybeUnreachable);
    return builder.GetCurPackage();
}

void ExecutePlugin::CHIRPtrToString(
    std::unordered_set<Function*>& srcCodeImportedFuncs,
    std::unordered_set<GlobalVar*>& srcCodeImportedVars,
    std::vector<Function*>& initFuncsForConstVar,
    std::unordered_map<std::string, Function*>& implicitFuncs,
    std::unordered_map<Block*, Terminator*>& maybeUnreachable)
{
    for (auto f : srcCodeImportedFuncs) {
        srcCodeImportedFuncNames.emplace(f->GetIdentifierWithoutPrefix());
    }
    srcCodeImportedFuncs.clear();
    for (auto v : srcCodeImportedVars) {
        srcCodeImportedVarNames.emplace(v->GetIdentifierWithoutPrefix());
    }
    srcCodeImportedVars.clear();
    for (auto f : initFuncsForConstVar) {
        initFuncsForConstVarNames.emplace_back(f->GetIdentifierWithoutPrefix());
    }
    initFuncsForConstVar.clear();
    for (auto& it : implicitFuncs) {
        implicitFuncNames.emplace(it.first);
    }
    implicitFuncs.clear();
    for (auto& it : maybeUnreachable) {
        auto funcName = it.first->GetTopLevelFunc()->GetIdentifierWithoutPrefix();
        auto blockName = it.first->GetIdentifierWithoutPrefix();
        // maybe a terminator is created and removed in AST2CHIR, so it doesn't have a parent block
        if (it.second->GetParentBlock()) {
            auto terminatorName = it.second->GetParentBlock()->GetIdentifierWithoutPrefix();
            unreachableBlockNames[funcName] = {blockName, terminatorName};
        }
    }
    maybeUnreachable.clear();
}

void ExecutePlugin::StringToCHIRPtr(
    std::unordered_set<Function*>& srcCodeImportedFuncs,
    std::unordered_set<GlobalVar*>& srcCodeImportedVars,
    std::vector<Function*>& initFuncsForConstVar,
    std::unordered_map<std::string, Function*>& implicitFuncs,
    std::unordered_map<Block*, Terminator*>& maybeUnreachable)
{
    auto chirPkg = builder.GetCurPackage();
    for (auto def : chirPkg->GetAllExtendDef()) {
        if (auto builtinType = DynamicCast<BuiltinType*>(def->GetExtendedType())) {
            GetBuiltinTypeWithVTable(*builtinType, builder)->AddExtend(*def);
        } else {
            auto customType = StaticCast<CustomType*>(def->GetExtendedType());
            auto customTypeDef = customType->GetCustomTypeDef();
            CJC_NULLPTR_CHECK(customTypeDef);
            CJC_ASSERT(customTypeDef->GetCustomKind() != CustomDefKind::TYPE_EXTEND);
            customTypeDef->AddExtend(*def);
        }
    }
    builder.UpdateTypeInCorePackage();

    for (auto& it : implicitFuncNames) {
        for (auto f : chirPkg->GetGlobalFuncsWithoutBody()) {
            if (f->GetIdentifierWithoutPrefix() == it) {
                implicitFuncs.emplace(it, f);
                break;
            }
        }
    }

    for (auto& it : srcCodeImportedFuncNames) {
        for (auto f : chirPkg->GetGlobalFuncsWithBody()) {
            if (f->GetIdentifierWithoutPrefix() == it) {
                srcCodeImportedFuncs.emplace(f);
                break;
            }
        }
    }

    for (auto& it : srcCodeImportedVarNames) {
        for (auto v : chirPkg->GetGlobalVarsWithInit()) {
            if (v->GetIdentifierWithoutPrefix() == it) {
                srcCodeImportedVars.emplace(v);
                break;
            }
        }
    }

    for (auto& it : initFuncsForConstVarNames) {
        for (auto f : chirPkg->GetGlobalFunctions()) {
            if (f->GetIdentifierWithoutPrefix() == it) {
                initFuncsForConstVar.emplace_back(f);
                break;
            }
        }
    }

    for (auto& it : unreachableBlockNames) {
        for (auto f : chirPkg->GetGlobalFuncsWithBody()) {
            if (f->GetIdentifierWithoutPrefix() != it.first) {
                continue;
            }
            auto allBlocks = f->GetBody()->GetAllBlocks();
            Block* bb = nullptr;
            Terminator* tt = nullptr;
            for (auto block : allBlocks) {
                if (bb != nullptr && tt != nullptr) {
                    break;
                }
                if (block->GetIdentifierWithoutPrefix() == it.second.first) {
                    bb = block;
                }
                if (block->GetIdentifierWithoutPrefix() == it.second.second) {
                    tt = block->GetTerminator();
                }
            }
            if (bb != nullptr && tt != nullptr) {
                maybeUnreachable.emplace(bb, tt);
            }
        }
    }
}

bool ExecutePlugin::FreeCachedData()
{
    auto fPtr = InvokeRuntime::GetMethod(handle, "freeSerializedMemory");
    if (!fPtr) {
        return false;
    }
    reinterpret_cast<void (*)()>(fPtr)();
    return true;
}