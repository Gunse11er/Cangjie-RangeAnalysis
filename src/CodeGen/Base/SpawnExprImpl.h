// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_SPAWNEXPRIMPL_H
#define CANGJIE_SPAWNEXPRIMPL_H
#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CodeGen {
class IRBuilder2;
class CHIRSpawnWrapper;
llvm::Value* GenerateSpawn(IRBuilder2& irBuilder, const CHIRSpawnWrapper& spawn);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_SPAWNEXPRIMPL_H
