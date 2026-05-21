// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CJNative/CGTypes/EnumCtorTIOrTTGenerator.h"

#include "Base/CGTypes/CGEnumType.h"
#include "CGModule.h"
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
#include "CJNative/CGTypeInfo.h"
#endif
#include "IRBuilder.h"
#include "cangjie/CHIR/IR/Type/EnumDef.h"

namespace Cangjie::CodeGen {
EnumCtorTIOrTTGenerator::EnumCtorTIOrTTGenerator(
    CGModule& cgMod, const CHIR::EnumType& chirEnumType, std::size_t ctorIndex)
    : cgMod(cgMod), cgCtx(cgMod.GetCGContext()), chirEnumType(chirEnumType), ctorIndex(ctorIndex)
{
}

llvm::Constant* EnumCtorTIOrTTGenerator::GenTypeArgsNumOfTypeInfo()
{
    return llvm::ConstantInt::get(llvm::Type::getInt8Ty(cgMod.GetLLVMContext()), chirEnumType.GetGenericArgs().size());
}

llvm::Constant* EnumCtorTIOrTTGenerator::GenTypeArgsOfTypeInfo()
{
    auto& llvmCtx = cgMod.GetLLVMContext();
    auto i8Ptr = llvm::Type::getInt8PtrTy(llvmCtx);
    auto typeArgsNum = chirEnumType.GetGenericArgs().size();
    if (!typeArgsNum) {
        return llvm::ConstantPointerNull::get(i8Ptr);
    }

    auto typeInfoPtrTy = CGType::GetOrCreateTypeInfoPtrType(llvmCtx);
    auto typeOfGenericArgsGV = llvm::ArrayType::get(typeInfoPtrTy, typeArgsNum);
    auto typeInfoOfGenericArgs = llvm::cast<llvm::GlobalVariable>(cgMod.GetLLVMModule()->getOrInsertGlobal(
        CGType::GetNameOfTypeInfoGV(chirEnumType) + ".typeArgs", typeOfGenericArgsGV));
    typeInfoOfGenericArgs->addAttribute(CJTI_TYPE_ARGS_ATTR);
    typeInfoOfGenericArgs->setConstant(true);
    typeInfoOfGenericArgs->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    AddLinkageTypeMetadata(*typeInfoOfGenericArgs, llvm::GlobalVariable::PrivateLinkage, cgCtx.IsCGParallelEnabled());
    return llvm::ConstantExpr::getBitCast(typeInfoOfGenericArgs, llvm::Type::getInt8PtrTy(llvmCtx));
}

llvm::Constant* EnumCtorTIOrTTGenerator::GenSourceGenericOfTypeInfo()
{
    auto p0i8 = llvm::Type::getInt8PtrTy(cgMod.GetLLVMContext());
    if (chirEnumType.GetCustomTypeDef()->IsGenericDef()) {
        auto genericDecl = chirEnumType.GetCustomTypeDef();
        return llvm::ConstantExpr::getBitCast(
            cgMod.GetOrCreateEnumCtorTIOrTT(*StaticCast<CHIR::EnumType*>(genericDecl->GetType()), ctorIndex), p0i8);
    } else {
        return llvm::ConstantPointerNull::get(p0i8);
    }
}

EnumCtorLayout EnumCtorTIOrTTGenerator::ComputeLLVMLayout(
    const std::vector<CHIR::Type*>& fields, const std::string& tiName, const std::string& className)
{
    EnumCtorLayout layout;
    layout.fieldTypes = fields;
    auto layoutType = GetLLVMStructType(cgMod, layout.fieldTypes, GetClassObjLayoutName(className));
    auto& dataLayout = cgMod.GetLLVMModule()->getDataLayout();
    layout.size = dataLayout.getTypeAllocSize(layoutType);
    layout.align = dataLayout.getABITypeAlignment(layoutType);
    layout.offsets = CGCustomType::GenOffsetsArray(cgMod, tiName + ".offsets", layoutType);
    return layout;
}
EnumCtorLayout EnumCtorTIOrTTGenerator::GenLayoutForReferenceType(const std::string& tiName,
                                                                  const std::string& className)
{
    EnumCtorLayout layout;
    layout.size = 8u;
    layout.align = 8u;
    auto cgEnumType = StaticCast<CGEnumType*>(CGType::GetOrCreate(cgMod, &chirEnumType));
    layout.fieldTypes.emplace_back(const_cast<CHIR::Type*>(&CGType::GetZeroSizedCGType(cgMod)->GetOriginal()));
    if (cgEnumType->IsOptionLikeT()) {
        layout.fieldTypes.emplace_back(CGType::GetRefTypeOfCHIRInt8(cgMod.GetCGContext().GetCHIRBuilder()));
    } else {
        const auto& ctors = chirEnumType.GetConstructorInfos(cgMod.GetCGContext().GetCHIRBuilder());
        CHIR::Type* associatedValueType = cgEnumType->IsAntiOptionLike() ? ctors[1].funcType->GetParamTypes()[0]
                                                                         : ctors[0].funcType->GetParamTypes()[0];
        layout.fieldTypes.emplace_back(associatedValueType);
    }
    auto layoutType = GetLLVMStructType(cgMod, layout.fieldTypes, GetClassObjLayoutName(className));
    layout.offsets = CGCustomType::GenOffsetsArray(cgMod, tiName + ".offsets", layoutType);
    return layout;
}

EnumCtorLayout EnumCtorTIOrTTGenerator::GenLayoutForZeroSize()
{
    EnumCtorLayout layout;
    auto i32Ty = llvm::Type::getInt32Ty(cgMod.GetLLVMContext());
    layout.size = 0;
    layout.align = 1;
    layout.offsets = llvm::ConstantPointerNull::get(i32Ty->getPointerTo());
    return layout;
}

EnumCtorLayout EnumCtorTIOrTTGenerator::GenLayoutForTrivial(const std::string& tiName)
{
    EnumCtorLayout layout;
    auto& llvmCtx = cgMod.GetLLVMContext();
    auto i32Ty = llvm::Type::getInt32Ty(llvmCtx);
    auto i32ArrType = llvm::ArrayType::get(i32Ty, 1u);
    layout.size = 4;
    layout.align = 4;
    std::string name = tiName + ".offsets";
    auto typeInfoOfFields =
        llvm::cast<llvm::GlobalVariable>(cgMod.GetLLVMModule()->getOrInsertGlobal(name, i32ArrType));
    typeInfoOfFields->setInitializer(llvm::ConstantArray::get(i32ArrType, {llvm::ConstantInt::get(i32Ty, 0)}));
    typeInfoOfFields->setLinkage(llvm::GlobalValue::LinkageTypes::PrivateLinkage);
    typeInfoOfFields->addAttribute(CJTI_OFFSETS_ATTR);
    layout.fieldTypes.emplace_back(cgMod.GetCGContext().GetCHIRBuilder().GetInt32Ty());
    layout.offsets = llvm::ConstantExpr::getBitCast(typeInfoOfFields, i32Ty->getPointerTo());
    return layout;
}

EnumCtorLayout EnumCtorTIOrTTGenerator::GenLayoutForStructure(const CGEnumType* cgEnumType,
    const std::vector<CHIR::Type*>& paramTypes, const std::string& tiName, const std::string& className)
{
    EnumCtorLayout layout;
    auto& ctx = cgMod.GetLLVMContext();
    auto i32Ty = llvm::Type::getInt32Ty(ctx);
    std::vector<CHIR::Type*> fields;
    if (cgEnumType->IsOptionLikeNonRef()) {
        fields.emplace_back(cgMod.GetCGContext().GetCHIRBuilder().GetBoolTy());
        fields.emplace_back(cgEnumType->GetAssociatedValueTypeOfOptionLike());
    } else {
        if (cgEnumType->IsOptionLike()) {
            fields.emplace_back(cgMod.GetCGContext().GetCHIRBuilder().GetBoolTy());
        } else {
            fields.emplace_back(cgMod.GetCGContext().GetCHIRBuilder().GetInt32Ty());
        }
        fields.insert(fields.end(), paramTypes.cbegin(), paramTypes.cend());
    }
    if (!cgEnumType->IsAllAssociatedValuesAreNonRef()) {
        return ComputeLLVMLayout(fields, tiName, className);
    } else { // EXHAUSTIVE_ASSOCIATED_NONREF
        layout.fieldTypes = fields;
        layout.align = 1;
        std::vector<llvm::Constant*> offSets(layout.fieldTypes.size());
        uint32_t totalSize = 0;
        for (size_t i = 0; i < layout.fieldTypes.size(); ++i) {
            auto fieldType = layout.fieldTypes[i];
            offSets[i] = llvm::ConstantInt::get(i32Ty, totalSize);
            auto cgField = CGType::GetOrCreate(cgMod, DeRef(*fieldType));
            auto fieldSize = cgField->GetSize().value_or(0);
            totalSize += fieldSize;
        }
        auto layoutType = GetLLVMStructType(cgMod, layout.fieldTypes, GetClassObjLayoutName(className));
        if (layoutType->elements().empty()) {
            layout.offsets = llvm::ConstantPointerNull::get(i32Ty->getPointerTo());
        } else {
            auto i32ArrType = llvm::ArrayType::get(i32Ty, layout.fieldTypes.size());
            std::string name = tiName + ".offsets";
            auto typeInfoOfFields =
                llvm::cast<llvm::GlobalVariable>(cgMod.GetLLVMModule()->getOrInsertGlobal(name, i32ArrType));
            typeInfoOfFields->setInitializer(llvm::ConstantArray::get(i32ArrType, offSets));
            typeInfoOfFields->setLinkage(llvm::GlobalValue::LinkageTypes::PrivateLinkage);
            typeInfoOfFields->addAttribute(CJTI_OFFSETS_ATTR);
            layout.offsets = llvm::ConstantExpr::getBitCast(typeInfoOfFields, i32Ty->getPointerTo());
        }
    }
    return layout;
}

void EnumCtorTIOrTTGenerator::GenerateNonGenericEnumCtorTypeInfo(llvm::GlobalVariable& ti)
{
    auto& llvmCtx = cgMod.GetLLVMContext();
    auto i8Ty = llvm::Type::getInt8Ty(llvmCtx);
    auto i16Ty = llvm::Type::getInt16Ty(llvmCtx);
    auto i32Ty = llvm::Type::getInt32Ty(llvmCtx);
    auto i8PtrNull = llvm::ConstantPointerNull::get(i8Ty->getPointerTo());

    auto tiName = ti.getName().str();
    auto className = tiName.substr(0, tiName.size() - 3); // 3: the length of ".ti" or ".tt"
    const auto& ctors = chirEnumType.GetConstructorInfos(cgMod.GetCGContext().GetCHIRBuilder());
    CJC_ASSERT(ctorIndex < ctors.size());
    const auto& paramTypes = ctors[ctorIndex].funcType->GetParamTypes();
    auto cgEnumType = StaticCast<CGEnumType*>(CGType::GetOrCreate(cgMod, &chirEnumType));
    EnumCtorLayout layout;
    if (cgEnumType->IsOptionLikeRef() || cgEnumType->IsOptionLikeT()) {
        // EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF || EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T
        layout = GenLayoutForReferenceType(tiName, className);
    } else if (cgEnumType->IsZeroSizeEnum()) {
        // EXHAUSTIVE_ZERO_SIZE
        layout = GenLayoutForZeroSize();
    } else if (cgEnumType->IsTrivial()) {
        // NON_EXHAUSTIVE_UNASSOCIATED || EXHAUSTIVE_UNASSOCIATED
        layout = GenLayoutForTrivial(tiName);
    } else {
        // NON_EXHAUSTIVE_ASSOCIATED || EXHAUSTIVE_OTHER || EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF ||
        // EXHAUSTIVE_ASSOCIATED_NONREF
        layout = GenLayoutForStructure(cgEnumType, paramTypes, tiName, className);
    }

    auto fieldsTypes = layout.fieldTypes;
    std::vector<llvm::Constant*> typeInfoVec(TYPE_INFO_FIELDS_NUM);
    typeInfoVec[static_cast<size_t>(TYPEINFO_NAME)] = cgMod.GenerateTypeNameConstantString(className, false);
    typeInfoVec[static_cast<size_t>(TYPEINFO_TYPE_KIND)] = llvm::ConstantInt::get(i8Ty, UGTypeKind::UG_COMMON_ENUM);
    typeInfoVec[static_cast<size_t>(TYPEINFO_FIELDS_NUM)] = llvm::ConstantInt::get(i16Ty, fieldsTypes.size());

    auto fieldConstants = CGCustomType::GenTypeInfoConstantVectorForTypes(cgMod, fieldsTypes);
    typeInfoVec[static_cast<size_t>(TYPEINFO_FIELDS)] =
        CGCustomType::GenTypeInfoArray(cgMod, tiName + ".fields", fieldConstants, CJTI_FIELDS_ATTR);

    auto layoutType = GetLLVMStructType(cgMod, fieldsTypes, GetClassObjLayoutName(className));
    typeInfoVec[static_cast<size_t>(TYPEINFO_SIZE)] = llvm::ConstantInt::get(i32Ty, layout.size);
    typeInfoVec[static_cast<size_t>(TYPEINFO_UUID)] = llvm::Constant::getNullValue(i32Ty);
    typeInfoVec[static_cast<size_t>(TYPEINFO_ALIGN)] = llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(layout.align));
    typeInfoVec[static_cast<size_t>(TYPEINFO_SOURCE_GENERIC)] = GenSourceGenericOfTypeInfo();
    typeInfoVec[static_cast<size_t>(TYPEINFO_TYPE_ARGS_NUM)] = GenTypeArgsNumOfTypeInfo();
    typeInfoVec[static_cast<size_t>(TYPEINFO_INHERITED_CLASS_NUM)] =
        llvm::ConstantInt::get(i16Ty, INHERITED_CLASS_NUM_FE_FLAG);
    typeInfoVec[static_cast<size_t>(TYPEINFO_OFFSETS)] = layout.offsets;
    typeInfoVec[static_cast<size_t>(TYPEINFO_TYPE_ARGS)] = GenTypeArgsOfTypeInfo();
    typeInfoVec[static_cast<size_t>(TYPEINFO_SUPER)] = CGType::GetOrCreate(cgMod, &chirEnumType)->GetOrCreateTypeInfo();
    typeInfoVec[static_cast<size_t>(TYPEINFO_EXTENSIONDEF_PTR)] =
        llvm::ConstantPointerNull::get(CGType::GetOrCreateExtensionDefPtrType(llvmCtx)->getPointerTo());
    typeInfoVec[static_cast<size_t>(TYPEINFO_MTABLE)] = i8PtrNull;
    typeInfoVec[static_cast<size_t>(TYPEINFO_REFLECTION)] = i8PtrNull;
    typeInfoVec[static_cast<size_t>(TYPEINFO_GC_TIB)] =
        llvm::ConstantPointerNull::get(CGType::GetBitMapType(llvmCtx)->getPointerTo());
    typeInfoVec[static_cast<size_t>(TYPEINFO_FLAG)] =
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(cgMod.GetLLVMContext()), 0);

    ti.setInitializer(llvm::ConstantStruct::get(CGType::GetOrCreateTypeInfoType(llvmCtx), typeInfoVec));
    ti.addAttribute(GC_KLASS_ATTR);
    if (cgCtx.GetCompileOptions().target.os != Triple::OSType::WINDOWS) {
        ti.setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
    }
    auto meta = llvm::MDTuple::get(llvmCtx, {llvm::MDString::get(llvmCtx, layoutType->getStructName().str())});
    ti.setMetadata(GC_TYPE_META_NAME, meta);
    if (!cgCtx.GetCompileOptions().disableReflection) {
        std::string mangledName = ctors[ctorIndex].annoInfo.mangledName;
        auto innerNode = llvm::MDTuple::get(
            llvmCtx, {llvm::MDString::get(llvmCtx, "enumCtor"), llvm::MDString::get(llvmCtx, mangledName)});
        auto outerNode = llvm::MDTuple::get(llvmCtx, {innerNode});
        ti.setMetadata("Reflection", outerNode);
    }
    // This line seems only for Parallel-Compilation:
    cgCtx.AddGeneratedStructType(layoutType->getStructName().str());
    cgCtx.RegisterStaticGIName(ti.getName());
}

llvm::Constant* EnumCtorTIOrTTGenerator::GenTypeArgsNumOfTypeTemplate()
{
    return llvm::ConstantInt::get(llvm::Type::getInt16Ty(cgMod.GetLLVMContext()), chirEnumType.GetGenericArgs().size());
}

llvm::Constant* EnumCtorTIOrTTGenerator::GenSuperFnOfTypeTemplate(const std::string& funcName)
{
    auto i8PtrTy = llvm::Type::getInt8PtrTy(cgMod.GetLLVMContext());
    std::vector<llvm::Type*> argTypes{
        llvm::Type::getInt32Ty(cgMod.GetLLVMContext()),
        CGType::GetOrCreateTypeInfoPtrType(cgMod.GetLLVMContext())->getPointerTo()
    };
    auto superTiFnType = llvm::FunctionType::get(i8PtrTy, argTypes, false);
    auto superTiFn =
        llvm::Function::Create(superTiFnType, llvm::Function::PrivateLinkage, funcName, cgMod.GetLLVMModule());
    superTiFn->addFnAttr("native-interface-fn");
    CodeGen::IRBuilder2 irBuilder(cgMod);
    auto entryBB = irBuilder.CreateEntryBasicBlock(superTiFn, "entry");
    irBuilder.SetInsertPoint(entryBB);
    auto tt = CGType::GetOrCreate(cgMod, &chirEnumType)->GetOrCreateTypeTemplate();
    auto ti = irBuilder.CallIntrinsicGetTypeInfo({tt, superTiFn->getArg(0), superTiFn->getArg(1)});
    irBuilder.CreateRet(irBuilder.CreateBitCast(ti, i8PtrTy));
    return llvm::ConstantExpr::getBitCast(superTiFn, i8PtrTy);
}

void EnumCtorTIOrTTGenerator::GenerateGenericEnumCtorTypeTemplate(llvm::GlobalVariable& tt)
{
    auto& llvmCtx = cgMod.GetLLVMContext();
    auto i8Ty = llvm::Type::getInt8Ty(llvmCtx);
    auto i8PtrTy = llvm::Type::getInt8PtrTy(llvmCtx);
    auto i16Ty = llvm::Type::getInt16Ty(llvmCtx);
    auto i8PtrNull = llvm::ConstantPointerNull::get(i8PtrTy);

    auto ttName = tt.getName().str();
    auto className = ttName.substr(0, ttName.size() - 3); // 3: the length of ".ti" or ".tt"
    const auto& ctors = chirEnumType.GetConstructorInfos(cgMod.GetCGContext().GetCHIRBuilder());
    CJC_ASSERT(ctorIndex < ctors.size());
    const auto& paramTypes = ctors[ctorIndex].funcType->GetParamTypes();
    auto cgEnumType = StaticCast<CGEnumType*>(CGType::GetOrCreate(cgMod, &chirEnumType));
    EnumCtorLayout layout;
    if (cgEnumType->IsOptionLikeRef() || cgEnumType->IsOptionLikeT()) {
        // EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_REF || EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_T
        layout = GenLayoutForReferenceType(ttName, className);
    } else if (cgEnumType->IsZeroSizeEnum()) {
        // EXHAUSTIVE_ZERO_SIZE
        layout = GenLayoutForZeroSize();
    } else if (cgEnumType->IsTrivial()) {
        // NON_EXHAUSTIVE_UNASSOCIATED || EXHAUSTIVE_UNASSOCIATED
        layout = GenLayoutForTrivial(ttName);
    } else {
        // NON_EXHAUSTIVE_ASSOCIATED || EXHAUSTIVE_OTHER || EXHAUSTIVE_ASSOCIATED_OPTION_LIKE_NONREF ||
        // EXHAUSTIVE_ASSOCIATED_NONREF
        layout = GenLayoutForStructure(cgEnumType, paramTypes, ttName, className);
    }
    std::vector<llvm::Constant*> typeTemplateVec(TYPE_TEMPLATE_FIELDS_NUM);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_NAME)] = cgMod.GenerateTypeNameConstantString(className, false);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_TYPE_KIND)] =
        llvm::ConstantInt::get(i8Ty, UGTypeKind::UG_COMMON_ENUM);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_TYPE_ARGS_NUM)] = GenTypeArgsNumOfTypeTemplate();
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_FIELDS_NUM)] =
        llvm::ConstantInt::get(i16Ty, layout.fieldTypes.size());

    std::unordered_map<const CHIR::GenericType*, size_t> localGenericParamIndicesMap;
    std::size_t genericTypeIdx = 0;
    for (auto typeArg : GetGenericArgsFromCHIRType(chirEnumType)) {
        auto genericType = typeArg.GetGenericType();
        localGenericParamIndicesMap.emplace(genericType, genericTypeIdx++);
    }
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_FIELDS_FNS)] = cgEnumType->IsOptionLikeT()
        ? cgEnumType->GenFieldsFnsOfTypeTemplateForOptionLikeT(cgMod, ttName)
        : CGTypeInfo::GenFieldsFnsOfTypeTemplate(cgMod, ttName, layout.fieldTypes, localGenericParamIndicesMap);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_SUPER_FN)] = GenSuperFnOfTypeTemplate(ttName + ".superTiFn");
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_FINALIZER)] = i8PtrNull;
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_REFLECTION)] = llvm::ConstantPointerNull::get(i8PtrTy);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_FLAG)] = llvm::ConstantInt::get(i8Ty, 0);
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_EXTENSIONDEF_PTR)] =
        llvm::ConstantPointerNull::get(CGType::GetOrCreateExtensionDefPtrType(llvmCtx)->getPointerTo());
    typeTemplateVec[static_cast<size_t>(TYPETEMPLATE_INHERITED_CLASS_NUM)] = llvm::ConstantInt::get(i16Ty, 0U);
    if (!cgCtx.GetCompileOptions().disableReflection) {
        std::string mangledName = ctors[ctorIndex].annoInfo.mangledName;
        auto innerNode = llvm::MDTuple::get(llvmCtx,
            {llvm::MDString::get(llvmCtx, "enumCtor"),llvm::MDString::get(llvmCtx, mangledName)});
        auto outerNode = llvm::MDTuple::get(llvmCtx, {innerNode});
        tt.setMetadata("Reflection", outerNode);
    }
    tt.setInitializer(llvm::ConstantStruct::get(CGType::GetOrCreateTypeTemplateType(llvmCtx), typeTemplateVec));
    tt.addAttribute(TYPE_TEMPLATE_ATTR);
}

void EnumCtorTIOrTTGenerator::EmitForDynamicGI()
{
    auto enumDef = chirEnumType.GetCustomTypeDef();
    auto tt = cgMod.GetOrCreateEnumCtorTIOrTT(chirEnumType, ctorIndex);
    if (tt->hasInitializer() || enumDef->TestAttr(CHIR::Attribute::IMPORTED) ||
        enumDef->TestAttr(CHIR::Attribute::NON_RECOMPILE)) {
        return;
    }
    const auto& subCHIRPkg = cgCtx.GetSubCHIRPackage();
    bool definedInOtherLLVMModule = subCHIRPkg.chirCustomDefs.find(enumDef) == subCHIRPkg.chirCustomDefs.end();
    if (definedInOtherLLVMModule) {
        return;
    }

    GenerateGenericEnumCtorTypeTemplate(*tt);
    auto linkageType = CHIRLinkage2LLVMLinkage(enumDef->Get<CHIR::LinkTypeInfo>());
    if (linkageType == llvm::GlobalValue::InternalLinkage) {
        linkageType = llvm::GlobalValue::PrivateLinkage;
    }
    AddLinkageTypeMetadata(*tt, linkageType, cgCtx.IsCGParallelEnabled());
}

void EnumCtorTIOrTTGenerator::EmitForStaticGI()
{
    auto ti = cgMod.GetOrCreateEnumCtorTIOrTT(chirEnumType, ctorIndex);
    if (ti->hasInitializer()) {
        return;
    }

    GenerateNonGenericEnumCtorTypeInfo(*ti);
    auto linkageType = cgCtx.GetCompileOptions().target.os != Triple::OSType::WINDOWS ? llvm::GlobalValue::LinkOnceODRLinkage : llvm::GlobalValue::PrivateLinkage;
    AddLinkageTypeMetadata(*ti, linkageType, false);
}

void EnumCtorTIOrTTGenerator::EmitForConcrete()
{
    auto enumDef = chirEnumType.GetCustomTypeDef();
    auto ti = cgMod.GetOrCreateEnumCtorTIOrTT(chirEnumType, ctorIndex);
    if (ti->hasInitializer() || enumDef->TestAttr(CHIR::Attribute::IMPORTED) ||
        enumDef->TestAttr(CHIR::Attribute::NON_RECOMPILE)) {
        return;
    }
    const auto& subCHIRPkg = cgCtx.GetSubCHIRPackage();
    bool definedInOtherLLVMModule = subCHIRPkg.chirCustomDefs.find(enumDef) == subCHIRPkg.chirCustomDefs.end();
    if (definedInOtherLLVMModule) {
        return;
    }

    GenerateNonGenericEnumCtorTypeInfo(*ti);
    auto linkageType = CHIRLinkage2LLVMLinkage(enumDef->Get<CHIR::LinkTypeInfo>());
    if (linkageType == llvm::GlobalValue::InternalLinkage) {
        linkageType = llvm::GlobalValue::PrivateLinkage;
    }
    if (cgCtx.GetCompileOptions().target.os != Triple::OSType::WINDOWS) {
        linkageType = llvm::GlobalValue::LinkOnceODRLinkage;
    }
    AddLinkageTypeMetadata(*ti, linkageType, cgCtx.IsCGParallelEnabled());
}

void EnumCtorTIOrTTGenerator::Emit()
{
    auto cgEnumType = CGType::GetOrCreate(cgMod, &chirEnumType);
    if (cgEnumType->IsDynamicGI()) {
        EmitForDynamicGI();
    } else if (cgEnumType->IsStaticGI()) {
        EmitForStaticGI();
    } else if (cgEnumType->IsConcrete()) {
        EmitForConcrete();
    } else {
        CJC_ASSERT(false && "shouldn't reach here.");
    }
}
} // namespace Cangjie::CodeGen
