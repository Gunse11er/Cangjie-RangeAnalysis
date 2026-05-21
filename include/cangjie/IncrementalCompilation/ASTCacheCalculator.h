// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_AST_CACHE_CALCULATOR_H
#define CANGJIE_AST_CACHE_CALCULATOR_H

#include "cangjie/AST/Node.h"
#include "cangjie/IncrementalCompilation/CompilationCache.h"

namespace Cangjie::IncrementalCompilation {

/// a class that computes the information necessary to be kept for further incremental compilation, and also
/// data that may later be used to analyse changed decls since last compilation.
class ASTCacheCalculator {
public:
    ASTCacheCalculator(const AST::Package& p, const std::pair<bool, bool>& srcInfo);
    ~ASTCacheCalculator();

    void Walk() const;

    RawMangled2DeclMap mangled2Decl{}; // RawMangledName -> Ptr<AST::Decl> map
    ASTCache ret{};
    std::unordered_set<Ptr<const AST::Decl>> duplicatedMangleNames{}; // decls with duplicate RawMangledName
    
    // store direct extends temporarily: direct extends with the same extended type and constraints are the same,
    // collect them while traversing the ast, compute their cache after extracting all direct extends with the
    // same RawMangledName
    std::unordered_map<RawMangledName, std::list<std::pair<Ptr<AST::ExtendDecl>, int>>> directExtends;
    std::vector<const AST::Decl*> order; // the order of global decls by which decls are written to the cache.
        // order of members need not record as they are recorded in the MemberCache struct
    FileMap fileMap;

private:
    OwnedPtr<class ASTCacheCalculatorImpl> impl;
};
} // namespace Cangjie::IncrementalCompilation

#endif
