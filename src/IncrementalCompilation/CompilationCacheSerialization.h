// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the AST Serialization related classes, which provides AST serialization capabilities.
 */

#ifndef CANGJIE_COMPILATION_CACHE_SERIALIZATION_H
#define CANGJIE_COMPILATION_CACHE_SERIALIZATION_H

#include <cstdint>
#include <flatbuffers/flatbuffers.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "cangjie/IncrementalCompilation/CompilationCache.h"
#include "cangjie/Modules/ASTSerialization.h"

namespace Cangjie {
using TVirtualDepOffset = flatbuffers::Offset<CachedASTFormat::VirtualDep>;
class HashedASTWriter {
public:
    HashedASTWriter() = default;
    ~HashedASTWriter() = default;

    // Export external decls of a package AST to a buffer.
    // NOTE: should notice bep.
    void SetImportSpecs(const AST::Package& package);
    void SetLambdaCounter(uint64_t counter);
    void SetEnvClassCounter(uint64_t counter);
    void SetStringLiteralCounter(uint64_t counter);
    void SetCompileArgs(const std::vector<std::string>& args);
    void SetVarAndFuncDependency(
        const std::vector<std::pair<Ptr<const AST::Decl>, std::vector<Ptr<const AST::Decl>>>>& varAndFuncDep);
    void SetCHIROptInfo(const OptEffectStrMap& optInfo);
    void SetVirtualFuncDep(const VirtualWrapperDepMap& depMap);
    void SetVarInitDep(const VarInitDepMap& depMap);
    void SetCCOutFuncs(const std::set<std::string>& funcs);
    void SetBitcodeFilesName(const std::vector<std::string>& bitcodeFiles);
    void SetSemanticInfo(const SemanticInfo& info);
    void WriteAllDecls(ASTCache&& ast, ASTCache&& imports, std::vector<const AST::Decl*>&& order);
    std::vector<uint8_t> AST2FB(const std::string& pkgName);

private:
    flatbuffers::FlatBufferBuilder builder{INITIAL_FILE_SIZE};
    std::vector<TStringOffset> bitcodeFilesName;
    std::vector<TStringOffset> compileArgs;
    std::vector<TDeclDepOffset> varAndFunc;
    std::vector<TEffectMapOffset> chirOptInfo;
    std::vector<TVirtualDepOffset> virtualFuncDep;
    std::vector<TVirtualDepOffset> varInitDep;
    std::vector<TStringOffset> ccOutFuncs;
    // NOTE: For incremental compilation 2.0. Above members will be removed later.
    flatbuffers::Offset<CachedASTFormat::SemanticInfo> semaUsages;
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CachedASTFormat::TopDecl>>> allAST;
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CachedASTFormat::TopDecl>>> importedDecls;
    uint64_t lambdaCounter = 0;
    uint64_t envClassCounter = 0;
    uint64_t stringLiteralCounter = 0;
    uint64_t specs = 0;
};

class HashedASTLoader {
public:
    explicit HashedASTLoader(std::vector<uint8_t>&& astData) : serializedData(std::move(astData))
    {
    }
    ~HashedASTLoader() = default;
    std::pair<bool, CompilationCache> DeserializeData(const RawMangled2DeclMap& mangledName2DeclMap);

private:
    bool VerifyData();
    ASTCache LoadCachedAST(const CachedASTFormat::HashedPackage& p);
    std::unordered_map<RawMangledName, TopLevelDeclCache> LoadImported(const CachedASTFormat::HashedPackage& p);
    static SemanticInfo LoadSemanticInfos(
        const CachedASTFormat::HashedPackage& hasedPackage, const RawMangled2DeclMap& mangledName2DeclMap);
    MemberDeclCache Load(const CachedASTFormat::MemberDecl& decl);
    TopLevelDeclCache Load(const CachedASTFormat::TopDecl& decl, bool srcPkg);

    std::vector<uint8_t> serializedData;
    // pair of mangledname and gvid, to be sorted by gvid
    std::unordered_map<std::string, std::vector<std::pair<RawMangledName, int>>> fileMap{};
};

} // namespace Cangjie
#endif // CANGJIE_COMPILATION_CACHE_SERIALIZATION_H
