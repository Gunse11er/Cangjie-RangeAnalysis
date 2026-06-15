// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_REPLACE_SRC_CODE_IMPORTED_VAL_H
#define CANGJIE_CHIR_REPLACE_SRC_CODE_IMPORTED_VAL_H

#include "cangjie/CHIR/IR/Package.h"

namespace Cangjie::CHIR {
class ReplaceSrcCodeImportedVal {
public:
    ReplaceSrcCodeImportedVal(Package& package);

    void Run(const std::unordered_set<Function*>& srcCodeImportedFuncs,
        const std::unordered_set<GlobalVar*>& srcCodeImportedVars, const std::unordered_set<ClassDef*>& uselessClasses,
        const std::unordered_set<Function*>& uselessLambda);

private:
    void RemoveUselessDefFromCC(
        const std::unordered_set<ClassDef*>& uselessClasses, const std::unordered_set<Function*>& uselessLambda);

private:
    Package& package;
};
} // namespace Cangjie::CHIR
#endif // CANGJIE_CHIR_REPLACE_SRC_CODE_IMPORTED_VAL_H