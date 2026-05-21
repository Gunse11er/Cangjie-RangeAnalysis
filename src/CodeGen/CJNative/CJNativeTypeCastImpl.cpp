// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/TypeCastImpl.h"

#include "IRBuilder.h"

namespace Cangjie {
namespace CodeGen {
llvm::Value* GenerateTupleTypeCast(IRBuilder2& irBuilder, const CGValue& srcCGValue, const CGType& targetCGType)
{
    return irBuilder.CreateBitOrPointerCast(srcCGValue.GetRawValue(),
        targetCGType.GetLLVMType()->getPointerTo(srcCGValue.GetLLVMType()->getPointerAddressSpace()));
}
} // namespace CodeGen
} // namespace Cangjie
