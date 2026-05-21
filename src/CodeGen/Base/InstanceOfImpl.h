// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_INSTANCEOFIMPL_H
#define CANGJIE_INSTANCEOFIMPL_H

#include "llvm/IR/Value.h"

namespace Cangjie {
namespace CHIR {
class InstanceOf;
}
namespace CodeGen {
class IRBuilder2;
llvm::Value* GenerateInstanceOf(IRBuilder2& irBuilder, const CHIR::InstanceOf& instanceOf);
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_INSTANCEOFIMPL_H
