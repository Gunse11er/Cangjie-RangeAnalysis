// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/CGTypes/CGGenericType.h"

#include "CGModule.h"

namespace Cangjie::CodeGen {
llvm::Type* CGGenericType::GenLLVMType()
{
    if (llvmType) {
        return llvmType;
    }
    return CGType::GetRefType(cgMod.GetLLVMContext());
}

void CGGenericType::CalculateSizeAndAlign()
{
    size = std::nullopt;
    align = std::nullopt;
}

void CGGenericType::GenContainedCGTypes()
{
    // No contained types
}

llvm::Constant* CGGenericType::GenUpperBoundsOfGenericType(std::string& uniqueName)
{
    auto p0i8 = llvm::Type::getInt8PtrTy(cgMod.GetLLVMContext());
    if (upperBounds.empty()) {
        return llvm::ConstantPointerNull::get(p0i8);
    }

    std::vector<llvm::Constant*> constants;
    for (auto upperBound : upperBounds) {
        auto cgTypeOfUpperBound = CGType::GetOrCreate(cgMod, DeRef(*upperBound));
        constants.emplace_back(llvm::ConstantExpr::getBitCast(cgTypeOfUpperBound->GetOrCreateTypeInfo(), p0i8));
    }
    auto typeOfUpperBoundsGV = llvm::ArrayType::get(p0i8, constants.size());
    auto typeInfoOfUpperBounds = llvm::cast<llvm::GlobalVariable>(cgMod.GetLLVMModule()->getOrInsertGlobal(
        uniqueName + ".upperBounds", typeOfUpperBoundsGV));
    typeInfoOfUpperBounds->setInitializer(llvm::ConstantArray::get(typeOfUpperBoundsGV, constants));
    typeInfoOfUpperBounds->setLinkage(llvm::GlobalValue::LinkageTypes::PrivateLinkage);
    typeInfoOfUpperBounds->addAttribute(CJTI_UPPER_BOUNDS_ATTR);
    return llvm::ConstantExpr::getBitCast(typeInfoOfUpperBounds, p0i8);
}

llvm::GlobalVariable* CGGenericType::GetOrCreateTypeInfo()
{
    CJC_ASSERT(chirType.IsGeneric());
    auto genericTypeName = StaticCast<const CHIR::GenericType&>(chirType).GetSrcCodeIdentifier();
    upperBounds = StaticCast<const CHIR::GenericType&>(chirType).GetUpperBounds();
    std::string uniqueName = cgMod.GetCGContext().GetGenericTypeUniqueName(genericTypeName, upperBounds);
    if (auto found = cgMod.GetLLVMModule()->getNamedGlobal(uniqueName + ".ti"); found) {
        return found;
    }

    auto genericTypeInfoType = GetOrCreateGenericTypeInfoType(cgMod.GetLLVMContext());
    auto genericTypeInfo = llvm::cast<llvm::GlobalVariable>(
        cgMod.GetLLVMModule()->getOrInsertGlobal(uniqueName + ".ti", genericTypeInfoType));
    unsigned typeInfoKind = UGTypeKind::UG_GENERIC;
    std::vector<llvm::Constant*> typeInfoVec(GENERIC_TYPE_INFO_FIELDS_NUM);
    typeInfoVec[static_cast<size_t>(GENERIC_TYPEINFO_NAME)] =
        cgMod.GenerateTypeNameConstantString(genericTypeName, false);
    typeInfoVec[static_cast<size_t>(GENERIC_TYPEINFO_TYPE_KIND)] =
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(cgMod.GetLLVMContext()), typeInfoKind);
    typeInfoVec[static_cast<size_t>(GENERIC_TYPEINFO_UPPERBOUNDS_NUM)] =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(cgMod.GetLLVMContext()), upperBounds.size());
    typeInfoVec[static_cast<size_t>(GENERIC_TYPEINFO_UPPERBOUNDS)] =
        GenUpperBoundsOfGenericType(uniqueName);
    genericTypeInfo->setInitializer(llvm::ConstantStruct::get(genericTypeInfoType, typeInfoVec));
    genericTypeInfo->setLinkage(llvm::GlobalValue::PrivateLinkage);
    genericTypeInfo->addAttribute(GENERIC_TYPEINFO_ATTR);
    return genericTypeInfo;
}
} // namespace Cangjie::CodeGen
