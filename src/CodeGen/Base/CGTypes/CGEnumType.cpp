// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/CGTypes/CGEnumType.h"

#include "CGContext.h"
#include "CGModule.h"
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
#include "CJNative/CGTypeInfo.h"
#endif
#include "Utils/CGUtils.h"
#include "cangjie/CHIR/IR/Type/EnumDef.h"
#include "cangjie/Option/Option.h"

namespace Cangjie::CodeGen {
namespace {
bool ContainsRefType(const llvm::Type& llvmType)
{
    if (llvmType.isPointerTy() && llvmType.getPointerAddressSpace() == 1U) {
        return true;
    } else if (auto structType = llvm::dyn_cast<const llvm::StructType>(&llvmType); structType) {
        for (auto& elementType : structType->elements()) {
            if (ContainsRefType(*elementType)) {
                return true;
            }
        }
    }
    return false;
}

bool HasRefAssociatedValue(CGModule& cgMod, const std::vector<CHIR::Type*>& paramTypes)
{
    for (auto associatedValueCHIRType : paramTypes) {
        auto associatedValueCGType = CGType::GetOrCreate(cgMod, associatedValueCHIRType);
        if (ContainsRefType(*associatedValueCGType->GetLLVMType())) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> TryComputeDefaultConstructorUnionSize(
    CGModule& cgMod, const std::vector<CHIR::Type*>& paramTypes)
{
    std::size_t accumulatedSize = 0U;
    for (auto associatedValueCHIRType : paramTypes) {
        auto associatedValueCGType = CGType::GetOrCreate(cgMod, associatedValueCHIRType);
        auto associatedValueSize = associatedValueCGType->GetSize();
        if (!associatedValueSize.has_value()) {
            return std::nullopt;
        }
        accumulatedSize += associatedValueSize.value();
    }
    return accumulatedSize;
}

std::optional<std::size_t> TryComputeAndroidArm32StructConstructorUnionSize(
    CGModule& cgMod, const std::vector<CHIR::Type*>& paramTypes)
{
    for (auto associatedValueCHIRType : paramTypes) {
        if (IsReferenceType(*associatedValueCHIRType, cgMod)) {
            continue;
        }
        auto associatedValueCGType = CGType::GetOrCreate(cgMod, associatedValueCHIRType);
        if (!associatedValueCGType->GetSize().has_value()) {
            return std::nullopt;
        }
    }
    std::vector<CHIR::Type*> fullFieldTypes;
    fullFieldTypes.reserve(paramTypes.size() + 1U);
    fullFieldTypes.emplace_back(cgMod.GetCGContext().GetCHIRBuilder().GetInt32Ty());
    fullFieldTypes.insert(fullFieldTypes.end(), paramTypes.begin(), paramTypes.end());
    auto fullLayout = CGEnumType::ComputeAssociatedNonRefLayout(cgMod, fullFieldTypes);
    return fullLayout.size - sizeof(uint32_t);
}

std::optional<std::size_t> TryComputeConstructorUnionSize(
    CGModule& cgMod, const std::vector<CHIR::Type*>& paramTypes)
{
    return CGEnumType::NeedAndroidArm32AlignedEnumLayout(cgMod.GetCGContext().GetCompileOptions().target)
        ? TryComputeAndroidArm32StructConstructorUnionSize(cgMod, paramTypes)
        : TryComputeDefaultConstructorUnionSize(cgMod, paramTypes);
}

CGEnumType::AssociatedNonRefLayout ComputeDefaultAssociatedNonRefLayout(
    CGModule& cgMod, const std::vector<CHIR::Type*>& fieldTypes)
{
    CGEnumType::AssociatedNonRefLayout layout;
    layout.offsets.reserve(fieldTypes.size());
    uint32_t currentOffset = 0U;
    for (auto fieldType : fieldTypes) {
        layout.offsets.emplace_back(currentOffset);
        auto cgFieldType = CGType::GetOrCreate(cgMod, DeRef(*fieldType));
        auto fieldSize = cgFieldType->GetSize();
        CJC_ASSERT(fieldSize.has_value());
        currentOffset += static_cast<uint32_t>(fieldSize.value());
    }
    layout.size = currentOffset;
    layout.align = 1U;
    return layout;
}

CGEnumType::AssociatedNonRefLayout ComputeAndroidArm32StructAssociatedNonRefLayout(
    CGModule& cgMod, const std::vector<CHIR::Type*>& fieldTypes)
{
    CGEnumType::AssociatedNonRefLayout layout;
    layout.offsets.reserve(fieldTypes.size());
    auto layoutType = CGEnumType::GetAndroidArm32AssociatedNonRefLayoutType(cgMod, fieldTypes);
    auto& dataLayout = cgMod.GetLLVMModule()->getDataLayout();
    auto structLayout = dataLayout.getStructLayout(layoutType);
    layout.size = static_cast<uint32_t>(structLayout->getSizeInBytes());
    layout.align = static_cast<uint32_t>(dataLayout.getABITypeAlignment(layoutType));
    for (size_t idx = 0; idx < fieldTypes.size(); ++idx) {
        layout.offsets.emplace_back(static_cast<uint32_t>(structLayout->getElementOffset(idx)));
    }
    return layout;
}

// check if enum has RefTypeAssociatedValue and get the size of constructor union.
std::tuple<bool, std::optional<std::size_t>> ObtainingAuxiliaryInformation(
    CGModule& cgMod, const CHIR::EnumType& chirEnumType)
{
    bool hasRefTypeAssociatedValue = false;
    std::optional<std::size_t> sizeOfConstructorUnion = std::nullopt;

    const auto& ctors = chirEnumType.GetConstructorInfos(cgMod.GetCGContext().GetCHIRBuilder());
    CJC_ASSERT_WITH_MSG(!ctors.empty(), "The enum type has at least one constructor.");
    for (auto ctor : ctors) {
        const auto& paramTypes = ctor.funcType->GetParamTypes();
        hasRefTypeAssociatedValue |= HasRefAssociatedValue(cgMod, paramTypes);
        auto sizeOfConstructorUnionTemp = TryComputeConstructorUnionSize(cgMod, paramTypes);
        if (sizeOfConstructorUnionTemp.has_value()) {
            sizeOfConstructorUnion = sizeOfConstructorUnion.value_or(0U) < sizeOfConstructorUnionTemp.value()
                ? sizeOfConstructorUnionTemp.value()
                : sizeOfConstructorUnion.value_or(0U);
        } else {
            sizeOfConstructorUnion = std::nullopt;
            break;
        }
    }
    return std::make_tuple(hasRefTypeAssociatedValue, sizeOfConstructorUnion);
}
}

llvm::StructType* CGEnumType::GetAndroidArm32AssociatedNonRefLayoutType(
    CGModule& cgMod, const std::vector<CHIR::Type*>& fieldTypes)
{
    std::vector<llvm::Type*> llvmFieldTypes;
    llvmFieldTypes.reserve(fieldTypes.size());
    for (auto fieldType : fieldTypes) {
        auto* derefType = DeRef(*fieldType);
        if (IsReferenceType(*derefType, cgMod)) {
            llvmFieldTypes.emplace_back(CGType::GetRefType(cgMod.GetLLVMContext()));
        } else {
            llvmFieldTypes.emplace_back(CGType::GetOrCreate(cgMod, derefType)->GetLLVMType());
        }
    }
    return llvm::StructType::get(cgMod.GetLLVMContext(), llvmFieldTypes, false);
}

CGEnumType::AssociatedNonRefLayout CGEnumType::ComputeAssociatedNonRefLayout(
    CGModule& cgMod, const std::vector<CHIR::Type*>& fieldTypes)
{
    return NeedAndroidArm32AlignedEnumLayout(cgMod.GetCGContext().GetCompileOptions().target)
        ? ComputeAndroidArm32StructAssociatedNonRefLayout(cgMod, fieldTypes)
        : ComputeDefaultAssociatedNonRefLayout(cgMod, fieldTypes);
}

CGEnumType::CGEnumType(CGModule& cgMod, CGContext& cgCtx, const CHIR::Type& chirType)
    : CGCustomType(cgMod, cgCtx, chirType, CGTypeKind::CG_ENUM),
      chirEnumType(StaticCast<const CHIR::EnumType&>(chirType)),
      optionLikeInfo(nullptr),
      sizeOfConstructorUnion(std::nullopt),
      cgEnumTypeKind(CalculateCGEnumTypeKind())
{
}

CGEnumType::CGEnumTypeKind CGEnumType::CalculateCGEnumTypeKind()
{
    // 1. check if enum is non_exhaustive
    if (!chirEnumType.GetEnumDef()->IsExhaustive()) {
        return chirEnumType.GetEnumDef()->IsAllCtorsTrivial() ? CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED
                                                      : CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED;
    }
    // 2. check if enum has RefTypeAssociatedValue and get the size of constructor union.
    auto [hasRefTypeAssociatedValue, sizeOfConstructorUnionTemp] = ObtainingAuxiliaryInformation(cgMod, chirEnumType);
    sizeOfConstructorUnion = sizeOfConstructorUnionTemp;
    // 3. determine other scenarios.
    const auto& ctors = chirEnumType.GetConstructorInfos(cgMod.GetCGContext().GetCHIRBuilder());
    if (ctors.size() == 1U && sizeOfConstructorUnion.has_value() && sizeOfConstructorUnion.value() == 0U &&
        chirEnumType.GetGenericArgs().empty()) {
        return CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE;
    }
    if (chirEnumType.GetEnumDef()->IsAllCtorsTrivial()) {
        return CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED;
    }
    if (ctors.size() == 2U && // OptionLike always has two constructors
        ((ctors[0].funcType->GetParamTypes().size() == 0U && ctors[1].funcType->GetParamTypes().size() == 1U) ||
        (ctors[0].funcType->GetParamTypes().size() == 1U && ctors[1].funcType->GetParamTypes().size() == 0U))) {
        bool isAntiOptionLike = ctors[0].funcType->GetParamTypes().empty();
        CHIR::Type* associatedValueType = isAntiOptionLike ? ctors[1].funcType->GetParamTypes()[0]
                                                            : ctors[0].funcType->GetParamTypes()[0];
        optionLikeInfo = std::make_unique<CGEnumType::OptionLikeInfo>(isAntiOptionLike, associatedValueType);

        if (associatedValueType->IsGeneric()) {
            return CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T;
        }
        auto baseTypeOfAssociatedValue = DeRef(*associatedValueType);
        if (baseTypeOfAssociatedValue->IsEnum() &&
            StaticCast<CGEnumType*>(CGType::GetOrCreate(cgMod, baseTypeOfAssociatedValue))->IsCommonEnum()) {
            return CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF;
        }
        if (baseTypeOfAssociatedValue->IsReferenceType()) {
            return CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF;
        } else {
            return CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF;
        }
    }
    if (!hasRefTypeAssociatedValue && chirEnumType.GetGenericArgs().empty()) {
        CJC_ASSERT(sizeOfConstructorUnion.has_value());
        return CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF;
    }
    return CGEnumTypeKind::EXHAUSTIVE_OTHER;
}

std::string CGEnumType::GetEnumTypeName() const
{
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED: {
            return ENUM_TYPE_PREFIX + "Trivial";
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            return ENUM_TYPE_PREFIX + chirEnumType.GetEnumDef()->GetIdentifierWithoutPrefix() + "<T>";
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            return ENUM_TYPE_PREFIX + "OptionLike.Ref";
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            return ENUM_TYPE_PREFIX + GetTypeQualifiedName(chirEnumType);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF: {
            return ENUM_TYPE_PREFIX + GetTypeQualifiedName(chirEnumType);
        }
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            return ENUM_TYPE_PREFIX + "Common";
        }
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            return ENUM_TYPE_PREFIX + "Empty";
        }
        default:
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            return "";
    }
}

llvm::Type* CGEnumType::GenLLVMType()
{
    if (llvmType) {
        return llvmType;
    }

    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED: {
            return GenTrivialLLVMType();
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF: {
            return GenAssociatedNonRefEnumLLVMType();
        }
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            return GenCommonEnumLLVMType();
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            return llvm::Type::getInt8Ty(cgCtx.GetLLVMContext());
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            return GenOptionLikeRefLLVMType();
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            return GenOptionLikeNonRefLLVMType();
        }
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            return GenZeroSizeEnumLLVMType();
        }
        default: {
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            return nullptr;
        }
    }
}

llvm::Type* CGEnumType::GenTrivialLLVMType()
{
    auto& llvmCtx = cgCtx.GetLLVMContext();
    const auto& typeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, typeName);
    if (!layoutType) {
        layoutType = llvm::StructType::create(llvmCtx, typeName);
        SetStructTypeBody(layoutType, {llvm::Type::getInt32Ty(llvmCtx)});
    }
    return llvm::Type::getInt32Ty(llvmCtx);
}

llvm::Type* CGEnumType::GenCommonEnumLLVMType()
{
    auto& llvmCtx = cgCtx.GetLLVMContext();
    const auto& typeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, typeName);
    if (!layoutType) {
        layoutType = llvm::StructType::create(llvmCtx, typeName);
        SetStructTypeBody(layoutType, {llvm::Type::getInt8PtrTy(llvmCtx, 1U)});
    }
    return GetRefType(llvmCtx);
}

llvm::Type* CGEnumType::GenZeroSizeEnumLLVMType()
{
    auto& llvmCtx = cgCtx.GetLLVMContext();
    const auto& typeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, typeName);
    if (!layoutType) {
        layoutType = llvm::StructType::create(llvmCtx, {}, typeName);
    }
    return layoutType;
}

llvm::Type* CGEnumType::GenOptionLikeRefLLVMType()
{
    auto& llvmCtx = cgCtx.GetLLVMContext();
    const auto& typeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, typeName);
    if (!layoutType) {
        layoutType = llvm::StructType::create(llvmCtx, typeName);
        auto i8Ty = llvm::Type::getInt8Ty(llvmCtx);
        SetStructTypeBody(layoutType, {llvm::Type::getInt8PtrTy(llvmCtx, 1U), llvm::ArrayType::get(i8Ty, 0U)});
    }
    cgCtx.AddGeneratedStructType(layoutType->getName().str());
    return GetRefType(llvmCtx);
}

llvm::Type* CGEnumType::GenOptionLikeNonRefLLVMType()
{
    CJC_NULLPTR_CHECK(optionLikeInfo);
    auto elemCGType = CGType::GetOrCreate(cgMod, optionLikeInfo->associatedValueType);
    auto& llvmCtx = cgCtx.GetLLVMContext();
    if (!elemCGType->GetSize()) {
        return llvm::Type::getInt8Ty(llvmCtx);
    }
    const auto& enumTypeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, enumTypeName);
    llvmType = layoutType;
    if (llvmType && cgCtx.IsGeneratedStructType(enumTypeName)) {
        return llvmType;
    } else if (!llvmType) {
        layoutType = llvm::StructType::create(llvmCtx, enumTypeName);
        llvmType = layoutType;
    }
    cgCtx.AddGeneratedStructType(enumTypeName);

    std::vector<llvm::Type*> elemTypes{};
    elemTypes.emplace_back(llvm::Type::getInt1Ty(llvmCtx));
    elemTypes.emplace_back(elemCGType->GetLLVMType());
    SetStructTypeBody(llvm::cast<llvm::StructType>(llvmType), elemTypes);

    return llvmType;
}

llvm::Type* CGEnumType::GenAssociatedNonRefEnumLLVMType()
{
    auto& llvmCtx = cgCtx.GetLLVMContext();
    CJC_ASSERT(sizeOfConstructorUnion.has_value());
    auto layoutSize = 4U + sizeOfConstructorUnion.value(); // 4U: size of int32
    llvmType = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvmCtx), layoutSize);

    const auto& enumTypeName = GetEnumTypeName();
    layoutType = llvm::StructType::getTypeByName(llvmCtx, enumTypeName);
    if (layoutType && cgCtx.IsGeneratedStructType(enumTypeName)) {
        return llvmType;
    } else if (!layoutType) {
        layoutType = llvm::StructType::create(llvmCtx, enumTypeName);
    }
    cgCtx.AddGeneratedStructType(enumTypeName);
    SetStructTypeBody(layoutType, std::vector<llvm::Type*>{llvmType});
    return llvmType;
}

void CGEnumType::GenContainedCGTypes()
{
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            containedCGTypes.emplace_back(CGType::GetInt32CGType(cgMod));
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF: {
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            // Can't be determined at compile time.
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            CJC_NULLPTR_CHECK(optionLikeInfo);
            containedCGTypes.emplace_back(CGType::GetOrCreate(cgMod, optionLikeInfo->associatedValueType));
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            CJC_NULLPTR_CHECK(optionLikeInfo);
            containedCGTypes.emplace_back(CGType::GetBoolCGType(cgMod));
            containedCGTypes.emplace_back(CGType::GetOrCreate(cgMod, optionLikeInfo->associatedValueType));
            break;
        }
        default: {
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            break;
        }
    }
}

void CGEnumType::CalculateSizeAndAlign()
{
    llvm::DataLayout layOut = cgMod.GetLLVMModule()->getDataLayout();
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED: {
            size = llvmType->getPrimitiveSizeInBits() / 8U;
            align = size;
            return;
        }
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER:
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            size = layOut.getTypeAllocSize(llvmType);
            align = layOut.getABITypeAlignment(llvmType);
            return;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF: {
            CJC_ASSERT(sizeOfConstructorUnion.has_value());
            size = 4U + sizeOfConstructorUnion.value(); // 4U: size of int32
            align = NeedAndroidArm32AlignedEnumLayout(cgCtx.GetCompileOptions().target) ? 4U : 1U;
            return;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            if (layoutType) {
                auto layOut = cgMod.GetLLVMModule()->getDataLayout();
                size = layOut.getTypeAllocSize(layoutType);
                align = layOut.getABITypeAlignment(layoutType);
            } else {
                size = std::nullopt;
                align = std::nullopt;
            }
            return;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            // size and align can't be determined at compile time
            size = std::nullopt;
            align = std::nullopt;
            return;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            size = 0U;
            align = 1U;
            return;
        }
        default:    // unexpected CGEnumTypeKind
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            return;
    }
}

llvm::Constant* CGEnumType::GenFieldsNumOfTypeInfo()
{
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 1U);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF:
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF:
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 2U);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF:
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 0U);
        }
        default:    // unexpected CGEnumTypeKind
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            return nullptr;
    }
}

llvm::Constant* CGEnumType::GenFieldsOfTypeInfo()
{
    std::vector<CHIR::Type*> fieldTypes;
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetInt32CGType(cgMod)->GetOriginal()));
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            CJC_NULLPTR_CHECK(optionLikeInfo);
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetUnitCGType(cgMod)->GetOriginal()));
            fieldTypes.emplace_back(optionLikeInfo->associatedValueType);
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            CJC_NULLPTR_CHECK(optionLikeInfo);
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetBoolCGType(cgMod)->GetOriginal()));
            fieldTypes.emplace_back(optionLikeInfo->associatedValueType);
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF:
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T:
        default:
            CJC_ASSERT_WITH_MSG(false, "Should not reach here");
            break;
    }
    std::vector<llvm::Constant*> fieldConstants = GenTypeInfoConstantVectorForTypes(cgMod, fieldTypes);
    return GenTypeInfoArray(cgMod, CGType::GetNameOfTypeInfoGV(chirType) + ".fields", fieldConstants, CJTI_FIELDS_ATTR);
}

llvm::Constant* CGEnumType::GenOffsetsOfTypeInfo()
{
    if (cgEnumTypeKind == CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF) {
        return llvm::ConstantPointerNull::get(llvm::Type::getInt32PtrTy(cgMod.GetLLVMContext()));
    } else {
        return CGCustomType::GenOffsetsOfTypeInfo();
    }
}

llvm::Constant* CGEnumType::GenFieldsNumOfTypeTemplate()
{
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 1U);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T:
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF:
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 2U);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), 0U);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF:
        default:
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            return nullptr;
    }
}

llvm::Constant* CGEnumType::GenFieldsFnsOfTypeTemplateForOptionLikeT(CGModule& cgMod, const std::string& funcPrefixName)
{
    auto& llvmCtx = cgMod.GetLLVMContext();
    auto p0i8 = llvm::Type::getInt8PtrTy(llvmCtx);
    if (auto gv = cgMod.GetLLVMModule()->getNamedGlobal(funcPrefixName + ".fieldTiFns");
        gv && gv->hasInitializer()) {
        return llvm::ConstantExpr::getBitCast(gv, p0i8);
    }

    auto typeInfoPtrType = CGType::GetOrCreateTypeInfoPtrType(llvmCtx);
    std::vector<llvm::Type*> argTypes{llvm::Type::getInt32Ty(llvmCtx), typeInfoPtrType->getPointerTo()};
    auto fieldFnType = llvm::FunctionType::get(p0i8, argTypes, false);

    // 1. create fieldTiFn 0
    auto getTiFn1 = llvm::Function::Create(
        fieldFnType, llvm::Function::PrivateLinkage, funcPrefixName + ".fieldTiFn.0", cgMod.GetLLVMModule());
    getTiFn1->addFnAttr("native-interface-fn");
    getTiFn1->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    CodeGen::IRBuilder2 irBuilder(cgMod);
    auto entryBB = irBuilder.CreateEntryBasicBlock(getTiFn1, "entry");
    irBuilder.SetInsertPoint(entryBB);
    auto ti = irBuilder.LLVMIRBuilder2::CreateLoad(typeInfoPtrType, getTiFn1->getArg(1));
    auto isRef = irBuilder.CreateTypeInfoIsReferenceCall(ti);
    auto [refBB, nonRefBB] = Vec2Tuple<2>(irBuilder.CreateAndInsertBasicBlocks({"ref", "nonRef"}));
    irBuilder.CreateCondBr(isRef, refBB, nonRefBB);
    irBuilder.SetInsertPoint(refBB);
    irBuilder.CreateRet(
        irBuilder.CreateBitCast(CGType::GetUnitCGType(cgMod)->GetOrCreateTypeInfo(), p0i8));
    irBuilder.SetInsertPoint(nonRefBB);
    irBuilder.CreateRet(irBuilder.CreateBitCast(CGType::GetBoolCGType(cgMod)->GetOrCreateTypeInfo(), p0i8));

    // 2. create fieldTiFn 1
    auto getTiFn2 = llvm::Function::Create(
        fieldFnType, llvm::Function::PrivateLinkage, funcPrefixName + ".fieldTiFn.1", cgMod.GetLLVMModule());
    getTiFn2->addFnAttr("native-interface-fn");
    getTiFn2->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    entryBB = irBuilder.CreateEntryBasicBlock(getTiFn2, "entry");
    irBuilder.SetInsertPoint(entryBB);
    ti = irBuilder.LLVMIRBuilder2::CreateLoad(typeInfoPtrType, getTiFn2->getArg(1));
    irBuilder.CreateRet(irBuilder.CreateBitCast(ti, p0i8));

    return CGTypeInfo::GenFieldsFnsOfTypeTemplate(cgMod, funcPrefixName, {getTiFn1, getTiFn2});
}

llvm::Constant* CGEnumType::GenFieldsFnsOfTypeTemplate()
{
    auto funcPrefixName = CGType::GetNameOfTypeTemplateGV(chirType);
    std::vector<CHIR::Type*> fieldTypes;
    std::unordered_map<const CHIR::GenericType*, size_t> localGenericParamIndicesMap;
    switch (cgEnumTypeKind) {
        case CGEnumTypeKind::NON_EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_UNASSOCIATED:
        case CGEnumTypeKind::NON_EXHAUSTIVE_ASSOCIATED:
        case CGEnumTypeKind::EXHAUSTIVE_OTHER: {
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetInt32CGType(cgMod)->GetOriginal()));
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF: {
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetUnitCGType(cgMod)->GetOriginal()));
            fieldTypes.emplace_back(
                CGType::GetRefTypeOf(cgCtx.GetCHIRBuilder(), CGType::GetObjectCGType(cgMod)->GetOriginal()));
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF: {
            CJC_NULLPTR_CHECK(optionLikeInfo);
            fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetBoolCGType(cgMod)->GetOriginal()));
            fieldTypes.emplace_back(optionLikeInfo->associatedValueType);
            std::size_t gTIdx = 0;
            for (auto typeArg : GetGenericArgsFromCHIRType(chirEnumType)) {
                auto gt = typeArg.GetGenericType();
                localGenericParamIndicesMap.emplace(gt, gTIdx++);
            }
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T: {
            return GenFieldsFnsOfTypeTemplateForOptionLikeT(cgMod, funcPrefixName);
        }
        case CGEnumTypeKind::EXHAUSTIVE_ZERO_SIZE: {
            break;
        }
        case CGEnumTypeKind::EXHAUSTIVE_ASSOCIATED_NONREF:
        default:
            CJC_ASSERT_WITH_MSG(false, "Should not reach here: UNKNOWN enum kind");
            break;
    }
    return CGTypeInfo::GenFieldsFnsOfTypeTemplate(cgMod, funcPrefixName, fieldTypes, localGenericParamIndicesMap);
}
} // namespace Cangjie::CodeGen
