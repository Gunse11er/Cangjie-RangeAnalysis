// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_INCREMENTALGEN2_H
#define CANGJIE_INCREMENTALGEN2_H

#include <map>
#include <unordered_set>

#include "llvm/Transforms/Utils/ValueMapper.h"

#include "cangjie/IncrementalCompilation/CachedMangleMap.h"

namespace Cangjie::CodeGen {
class IncrementalGen {
public:
    explicit IncrementalGen(bool cgParallelEnabled);
    ~IncrementalGen() = default;

    bool Init(const std::string& cachedIRPath, llvm::LLVMContext& llvmContext);
    llvm::Module* LinkModules(llvm::Module* incremental, const CachedMangleMap& cachedMangles = {});
    std::vector<std::string> GetIncrLLVMUsedNames();
    std::vector<std::string> GetIncrCachedStaticGINames();

private:
    void InitCodeGenAddedCachedMap();
    void UpdateCachedDeclsFromInjectedModule(const CachedMangleMap& cachedMangles);
    void CopyDeclarationsToInjectedModule();
    void FillValueMap(llvm::ValueToValueMapTy& valueMap);
    void UpdateInitializationsOfGlobalVariables(llvm::ValueToValueMapTy& valueMap);
    void UpdateDefinitionsOfFunction(llvm::ValueToValueMapTy& valueMap);
    void UpdateBodyOfKeepTypesFunction(llvm::ValueToValueMapTy& valueMap);
    void CollectUselessFunctions();
    void EraseUselessFunctions();
    void CollectUselessDefinitions(llvm::GlobalObject* uselessDefinition);
    void UpdateReflectionMetadata();
    void UpdateCodeGenAddedMetadata();
    void UpdateIncrLLVMUsedNames();

    const bool cgParallelEnabled;
    std::unique_ptr<llvm::Module> incrementalModule;
    std::unique_ptr<llvm::Module> injectedModule;
    // Key: decl name from CHIR
    // Value: codegen added variable name or codegen added function name for specific decl name
    std::unordered_map<std::string, std::unordered_set<std::string>> codegenAddedCachedMap;
    std::unordered_set<llvm::GlobalObject*> uselessDefinitions;
    std::unordered_set<llvm::GlobalObject*> deferErase;
    std::vector<std::string> llvmUsedGVNames;
    std::vector<std::string> staticGINames;
};
} // namespace Cangjie::CodeGen

#endif // CANGJIE_INCREMENTALGEN2_H
