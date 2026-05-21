// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_COLLECT_LOCAL_CONST_DECL_H
#define CANGJIE_CHIR_COLLECT_LOCAL_CONST_DECL_H

#include <vector>

#include "cangjie/AST/Node.h"

namespace Cangjie::CHIR {
class CollectLocalConstDecl {
public:
    /**
    * @brief collect local const decls
    *
    * @param decls AST decls
    * @param rootIsGlobalDecl 1st param is global or local decl
    */
    void Collect(const std::vector<Ptr<const AST::Decl>>& decls, bool rootIsGlobalDecl);
    const std::vector<const AST::VarDecl*>& GetLocalConstVarDecls() const;
    const std::vector<const AST::FuncDecl*>& GetLocalConstFuncDecls() const;

private:
    std::vector<const AST::VarDecl*> localConstVarDecls;
    std::vector<const AST::FuncDecl*> localConstFuncDecls;
};
}

#endif