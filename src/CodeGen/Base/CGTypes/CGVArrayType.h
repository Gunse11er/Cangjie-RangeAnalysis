// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CGVARRYTYPE_H
#define CANGJIE_CGVARRYTYPE_H

#include "Base/CGTypes/CGType.h"

namespace Cangjie {
namespace CodeGen {
class CGVArrayType : public CGType {
    friend class CGTypeMgr;

protected:
    llvm::Type* GenLLVMType() override;
    void GenContainedCGTypes() override;

private:
    CGVArrayType() = delete;

    explicit CGVArrayType(CGModule& cgMod, CGContext& cgCtx, const CHIR::Type& chirType)
        : CGType(cgMod, cgCtx, chirType)
    {
    }

    llvm::Constant* GenFieldsNumOfTypeInfo() override;
    llvm::Constant* GenFieldsOfTypeInfo() override;
    llvm::Constant* GenSourceGenericOfTypeInfo() override;
    llvm::Constant* GenTypeArgsNumOfTypeInfo() override;
    llvm::Constant* GenOffsetsOfTypeInfo() override;
    llvm::Constant* GenTypeArgsOfTypeInfo() override;
    llvm::Constant* GenSuperOfTypeInfo() override;

    void CalculateSizeAndAlign() override;
};
} // namespace CodeGen
} // namespace Cangjie
#endif // CANGJIE_CGVARRYTYPE_H
