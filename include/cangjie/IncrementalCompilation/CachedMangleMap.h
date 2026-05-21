// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_INCREMENTAL_COMPILATION_CACHED_MANGLE_MAP
#define CANGJIE_INCREMENTAL_COMPILATION_CACHED_MANGLE_MAP

#include <string>
#include <unordered_set>
#include "cangjie/AST/Node.h"
#include "cangjie/IncrementalCompilation/IncrementalCompilationLogger.h"

/// Describes which decls are new or removed compared to last compilation. The decls are recorded by their mangled
/// names.
struct CachedMangleMap {
    // Stored mangledName of decls which need be removed from IR.
    // NOTE: mangled names should be CodeGen recognizable.
    std::unordered_set<std::string> incrRemovedDecls;
    // imported inline decls, to be set external and non-dso_local
    std::unordered_set<std::string> importedInlineDecls;
    std::unordered_set<std::string> newExternalDecls;
    void EmplaceImportedInlineDeclPtr(const Cangjie::AST::Decl &decl)
    {
        importedInlineDeclsPtr.emplace(&decl);
    };
    void Clear()
    {
        incrRemovedDecls.clear();
        importedInlineDecls.clear();
        newExternalDecls.clear();
        importedInlineDeclsPtr.clear();
    }
    void UpdateImportedInlineDeclsMangle()
    {
        importedInlineDecls.clear();
        for (const auto& p : importedInlineDeclsPtr) {
            CJC_NULLPTR_CHECK(p);
            if (p) {
                importedInlineDecls.emplace(p->mangledName);
            }
        }
    }
    void Dump() const
    {
        auto& logger = IncrementalCompilationLogger::GetInstance();
        if (!logger.IsEnable()) {
            return;
        }
        if (incrRemovedDecls.empty() && importedInlineDecls.empty() && newExternalDecls.empty()) {
            logger.LogLn("[CachedMangleMap] empty");
            return;
        }
        logger.LogLn("[CachedMangleMap] START");
        if (!incrRemovedDecls.empty()) {
            logger.LogLn("[incrRemovedDecls]:");
            for (auto incrRemovedDecl : incrRemovedDecls) {
                logger.LogLn(incrRemovedDecl);
            }
        }
        if (!importedInlineDecls.empty()) {
            logger.LogLn("[importedInlineDecls]:");
            for (auto importedInlineDecl : importedInlineDecls) {
                logger.LogLn(importedInlineDecl);
            }
        }
        if (!newExternalDecls.empty()) {
            logger.LogLn("[newExternalDecls]:");
            for (auto newExternalDecl : newExternalDecls) {
                logger.LogLn(newExternalDecl);
            }
        }
        logger.LogLn("[CachedMangleMap] END");
    }
private:
    std::unordered_set<const Cangjie::AST::Decl*> importedInlineDeclsPtr;
};

#endif // CANGJIE_INCREMENTAL_COMPILATION_CACHED_MANGLE_MAP