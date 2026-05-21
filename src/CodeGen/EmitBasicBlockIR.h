// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_EMITBASICBLOCKIR_H
#define CANGJIE_EMITBASICBLOCKIR_H

namespace Cangjie {
namespace CHIR {
class Block;
}
namespace CodeGen {
class CGModule;
void EmitBasicBlockIR(CGModule& cgMod, const CHIR::Block& chirBB);
} // namespace CodeGen
} // namespace Cangjie

#endif // CANGJIE_EMITBASICBLOCKIR_H
