// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_EMITEXPRESSIONIR_H
#define CANGJIE_EMITEXPRESSIONIR_H

#include <vector>

namespace Cangjie {
namespace CHIR {
class Expression;
}
namespace CodeGen {
class CGModule;
void EmitExpressionIR(CGModule& cgMod, std::vector<CHIR::Expression*> chirExprs);
} // namespace CodeGen
} // namespace Cangjie

#endif // CANGJIE_EMITEXPRESSIONIR_H
