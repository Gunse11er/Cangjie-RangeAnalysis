// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_EMITFUNCTIONIR_H
#define CANGJIE_EMITFUNCTIONIR_H

#include <vector>

namespace Cangjie {
namespace CHIR {
class Func;
class ImportedFunc;
} // namespace CHIR

namespace CodeGen {
class CGModule;
void EmitFunctionIR(CGModule& cgMod, const std::vector<CHIR::Func*>& chirFuncs);
void EmitImportedCFuncIR(CGModule& cgMod, const std::vector<CHIR::ImportedFunc*>& importedCFuncs);
} // namespace CodeGen
} // namespace Cangjie

#endif // CANGJIE_EMITFUNCTIONIR_H
