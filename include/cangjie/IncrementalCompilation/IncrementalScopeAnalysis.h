// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_INCRE_SCOPE_ANALYSIS_H
#define CANGJIE_INCRE_SCOPE_ANALYSIS_H

#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cangjie/AST/Node.h"
#include "cangjie/IncrementalCompilation/CompilationCache.h"
#include "cangjie/Modules/ImportManager.h"

namespace Cangjie {

enum class IncreKind { NO_CHANGE, INCR, ROLLBACK, EMPTY_PKG, INVALID };

struct IncreResult {
    IncreKind kind;
    std::unordered_set<Ptr<AST::Decl>> declsToRecompile;
    std::list<RawMangledName> deleted;
    std::list<std::string> deletedMangleNames; // deleted mangle names for codegen
    CompilationCache cacheInfo;
    RawMangled2DeclMap mangle2decl;
    std::list<RawMangledName> reBoxedTypes;
    void Dump() const;
};

// A helper struct to organize the args of incremental scope analysis entry function
struct IncrementalScopeAnalysisArgs {
    const RawMangled2DeclMap rawMangleName2DeclMap;
    ASTCache&& astCacheInfo;
    const AST::Package& srcPackage;
    const GlobalOptions& op;
    const ImportManager& importer;
    const CompilationCache& cachedInfo;
    const FileMap& fileMap;
    std::unordered_map<RawMangledName, std::list<std::pair<Ptr<AST::ExtendDecl>, int>>>&& directExtends;
};

// Entry function of incremental scope analysis
IncreResult IncrementalScopeAnalysis(IncrementalScopeAnalysisArgs&& args);
} // namespace Cangjie
#endif // CANGJIE_INCRE_SCOPE_ANALYSIS_H
