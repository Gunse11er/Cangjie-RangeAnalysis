// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_TRANSFORMATION_EXECUTE_PLUGIN_H
#define CANGJIE_CHIR_TRANSFORMATION_EXECUTE_PLUGIN_H

#include "cangjie/CHIR/IR/CHIRBuilder.h"
#include <flatbuffers/detached_buffer.h>

namespace Cangjie {
namespace CHIR {
class ExecutePlugin {
public:
    explicit ExecutePlugin(CHIRBuilder& builder);
    ~ExecutePlugin();
    bool SerializePackage(const Package& package);
    bool Execute(const std::string& pluginPath);
    Package* DeserializePluginResult(
        std::unordered_set<Function*>& srcCodeImportedFuncs,
        std::unordered_set<GlobalVar*>& srcCodeImportedVars,
        std::vector<Function*>& initFuncsForConstVar,
        std::unordered_map<std::string, Function*>& implicitFuncs,
        std::unordered_map<Block*, Terminator*>& maybeUnreachable);
    bool FreeCachedData();
private:
    void CHIRPtrToString(
        std::unordered_set<Function*>& srcCodeImportedFuncs,
        std::unordered_set<GlobalVar*>& srcCodeImportedVars,
        std::vector<Function*>& initFuncsForConstVar,
        std::unordered_map<std::string, Function*>& implicitFuncs,
        std::unordered_map<Block*, Terminator*>& maybeUnreachable);
    void StringToCHIRPtr(
        std::unordered_set<Function*>& srcCodeImportedFuncs,
        std::unordered_set<GlobalVar*>& srcCodeImportedVars,
        std::vector<Function*>& initFuncsForConstVar,
        std::unordered_map<std::string, Function*>& implicitFuncs,
        std::unordered_map<Block*, Terminator*>& maybeUnreachable);

    struct PluginResult {
        uint8_t* data{nullptr};
        int64_t size{0};
        bool success{false};
    };
    CHIRBuilder& builder;
    void* handle{nullptr};
    flatbuffers::DetachedBuffer serializedData;
    PluginResult pluginResult;

    std::unordered_set<std::string> srcCodeImportedFuncNames;
    std::unordered_set<std::string> srcCodeImportedVarNames;
    std::vector<std::string> initFuncsForConstVarNames;
    std::unordered_set<std::string> implicitFuncNames;
    std::unordered_map<std::string, std::pair<std::string, std::string>> unreachableBlockNames;
};
}
}
#endif