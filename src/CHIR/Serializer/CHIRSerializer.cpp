// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Serializer/CHIRSerializer.h"
#include "CHIRSerializerImpl.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/IntrinsicKind.h"
#include "cangjie/CHIR/Utils/ToStringUtils.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/EnumDef.h"
#include "cangjie/CHIR/IR/Type/ExtendDef.h"
#include "cangjie/CHIR/IR/Type/StructDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/Utils/UserDefinedType.h"
#include "cangjie/Utils/ICEUtil.h"
#include "flatbuffers/PackageFormat_generated.h"
#include "flatbuffers/detached_buffer.h"

#include <algorithm>
#include <set>
#include <unordered_set>
#include <vector>

using namespace Cangjie::CHIR;

void CHIRSerializer::Serialize(const Package& package, const std::string filename, ToCHIR::Phase phase)
{
    Utils::ProfileRecorder recorder("CHIR", "serialization: " + PhaseToString(phase));
    CHIRSerializerImpl serializer(package);
    serializer.Initialize();
    serializer.Dispatch();
    serializer.Save(filename, phase);
}

flatbuffers::DetachedBuffer CHIRSerializer::Serialize(const Package& package)
{
    CHIRSerializerImpl serializer(package);
    serializer.Initialize();
    serializer.Dispatch();
    return serializer.ConvertToMemoryData();
}

// ========================== ID Fetchers ==============================

template <typename T, typename E> std::vector<uint32_t> CHIRSerializer::CHIRSerializerImpl::GetId(std::vector<E*> vec)
{
    std::vector<uint32_t> indices;
    for (E* elem : vec) {
        uint32_t id = GetId<T>(static_cast<const T*>(elem));
        indices.emplace_back(id);
    }
    return indices;
}

template <typename T, typename E>
std::vector<uint32_t> CHIRSerializer::CHIRSerializerImpl::GetId(std::vector<Ptr<E>> vec)
{
    std::vector<uint32_t> indices;
    for (Ptr<E> elem : vec) {
        uint32_t id = GetId<T>(static_cast<const T*>(elem.get()));
        indices.emplace_back(id);
    }
    return indices;
}

template <typename T, typename E>
std::vector<uint32_t> CHIRSerializer::CHIRSerializerImpl::GetId(const std::unordered_set<E*>& set) const
{
    std::vector<uint32_t> indices;
    for (E* elem : set) {
        uint32_t id = GetId<T>(static_cast<const T*>(elem));
        indices.emplace_back(id);
    }
    return indices;
}

template <> uint32_t CHIRSerializer::CHIRSerializerImpl::GetId(const Value* obj)
{
    if (value2Id.count(obj) == 0) {
        value2Id[obj] = ++valueCount;
        allValue.emplace_back(0);
        valueKind.emplace_back(0);
        valueQueue.push_back(obj);
    }
    return value2Id[obj];
}

template <> uint32_t CHIRSerializer::CHIRSerializerImpl::GetId(const Type* obj)
{
    if (type2Id.count(obj) == 0) {
        type2Id[obj] = ++typeCount;
        allType.emplace_back(0);
        typeKind.emplace_back(0);
        typeQueue.push(obj);
    }
    return type2Id[obj];
}

template <> uint32_t CHIRSerializer::CHIRSerializerImpl::GetId(const Expression* obj)
{
    if (expr2Id.count(obj) == 0) {
        expr2Id[obj] = ++exprCount;
        allExpression.emplace_back(0);
        exprKind.emplace_back(0);
        exprQueue.push(obj);
    }
    return expr2Id[obj];
}

template <> uint32_t CHIRSerializer::CHIRSerializerImpl::GetId(const CustomTypeDef* obj)
{
    if (def2Id.count(obj) == 0) {
        def2Id[obj] = ++defCount;
        allCustomTypeDef.emplace_back(0);
        defKind.emplace_back(0);
        defQueue.push_back(obj);
    }
    return def2Id[obj];
}

// ========================== Helper Serializers ===============================
template <typename FBT, typename T>
std::vector<flatbuffers::Offset<FBT>> CHIRSerializer::CHIRSerializerImpl::SerializeSetToVec(
    const std::unordered_set<T>& set) const
{
    std::vector<flatbuffers::Offset<FBT>> retval;
    for (T elem : set) {
        retval.emplace_back(Serialize<FBT>(elem));
    }
    return retval;
}

template <typename FBT, typename T>
std::vector<flatbuffers::Offset<FBT>> CHIRSerializer::CHIRSerializerImpl::SerializeVec(const std::vector<T>& vec)
{
    std::vector<flatbuffers::Offset<FBT>> retval;
    for (T elem : vec) {
        retval.emplace_back(Serialize<FBT>(elem));
    }
    return retval;
}

template <>
flatbuffers::Offset<PackageFormat::DebugLocation> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const DebugLocation& obj)
{
    auto beginPos = PackageFormat::CreatePos(builder, obj.GetBeginPos().line, obj.GetBeginPos().column);
    auto endPos = PackageFormat::CreatePos(builder, obj.GetEndPos().line, obj.GetEndPos().column);
    auto scope = obj.GetScopeInfo();
    return PackageFormat::CreateDebugLocationDirect(
        builder, obj.GetAbsPath().c_str(), obj.GetFileID(), beginPos, endPos, &scope);
}

template <>
flatbuffers::Offset<PackageFormat::AnnoInfo> CHIRSerializer::CHIRSerializerImpl::Serialize(const AnnoInfo& obj)
{
    std::vector<flatbuffers::Offset<PackageFormat::CustomAnnoInstance>> instances;
    for (const auto& inst : obj.GetCustomAnnoInstances()) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> argStrs;
        for (const auto& arg : inst.GetArgValues()) {
            argStrs.push_back(builder.CreateSharedString(arg));
        }
        auto locOff = Serialize<PackageFormat::DebugLocation>(inst.GetDebugLocation());
        instances.push_back(PackageFormat::CreateCustomAnnoInstanceDirect(builder,
            inst.GetAnnoClassName().data(), argStrs.empty() ? nullptr : &argStrs, locOff));
    }
    return PackageFormat::CreateAnnoInfoDirect(builder, obj.GetAnnoFactoryFuncMangledName().data(),
        instances.empty() ? nullptr : &instances);
}

[[maybe_unused]] static void Empty(Annotation*)
{
}

template <> flatbuffers::Offset<PackageFormat::Base> CHIRSerializer::CHIRSerializerImpl::Serialize(const Base& obj)
{
    auto annoTypes = std::vector<uint8_t>();
    auto annos = std::vector<flatbuffers::Offset<void>>();
    std::unordered_map<std::type_index, std::function<void(Annotation*)>> annoHandler;

    // NeedCheckArrayBound
    annoHandler[typeid(CHIR::NeedCheckArrayBound)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_needCheckArrayBound);
        annos.emplace_back(PackageFormat::CreateNeedCheckArrayBound(
            builder, NeedCheckArrayBound::Extract(StaticCast<NeedCheckArrayBound*>(anno)))
                .Union());
    };

    // NeedCheckCast
    annoHandler[typeid(CHIR::NeedCheckCast)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_needCheckCast);
        annos.emplace_back(
            PackageFormat::CreateNeedCheckCast(builder, NeedCheckCast::Extract(StaticCast<NeedCheckCast*>(anno)))
                .Union());
    };

    // DebugLocationInfoForWarning
    annoHandler[typeid(CHIR::DebugLocationInfoForWarning)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_debugLocationInfoForWarning);
        annos.emplace_back(Serialize<PackageFormat::DebugLocation>(
            DebugLocationInfoForWarning::Extract(StaticCast<DebugLocationInfoForWarning*>(anno)))
                .Union());
    };

    // LinkTypeInfo
    annoHandler[typeid(CHIR::LinkTypeInfo)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_linkTypeInfo);
        annos.emplace_back(PackageFormat::CreateLinkTypeInfo(
            builder, PackageFormat::Linkage(LinkTypeInfo::Extract(StaticCast<CHIR::LinkTypeInfo*>(anno))))
                .Union());
    };

    // SkipCheck
    annoHandler[typeid(CHIR::SkipCheck)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_skipCheck);
        annos.emplace_back(PackageFormat::CreateSkipCheck(
            builder, PackageFormat::SkipKind(SkipCheck::Extract(StaticCast<CHIR::SkipCheck*>(anno))))
                .Union());
    };

    // WrappedRawMethod may be removed body when removeUnusedImported, do not serializer it
    auto wrapMethod = dynamic_cast<Function*>(obj.Get<CHIR::WrappedRawMethod>());
    if (wrapMethod != nullptr && !wrapMethod->GetBody()) {
        annoHandler[typeid(CHIR::WrappedRawMethod)] = Empty;
    } else {
        annoHandler[typeid(CHIR::WrappedRawMethod)] = [this, &annos, &annoTypes](Annotation* anno) {
            annoTypes.push_back(PackageFormat::Annotation::Annotation_wrappedRawMethod);
            auto rawMethod =
                GetId<Value>(StaticCast<Value*>(WrappedRawMethod::Extract(StaticCast<CHIR::WrappedRawMethod*>(anno))));
            annos.emplace_back(PackageFormat::CreateWrappedRawMethod(builder, rawMethod).Union());
        };
    }
    // NeverOverflowInfo
    annoHandler[typeid(CHIR::NeverOverflowInfo)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_neverOverflowInfo);
        annos.emplace_back(PackageFormat::CreateNeverOverflowInfo(
            builder, NeverOverflowInfo::Extract(StaticCast<CHIR::NeverOverflowInfo*>(anno)))
                .Union());
    };

    // GeneratedFromForIn
    annoHandler[typeid(CHIR::GeneratedFromForIn)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_generatedFromForIn);
        annos.emplace_back(PackageFormat::CreateGeneratedFromForIn(
            builder, GeneratedFromForIn::Extract(StaticCast<CHIR::GeneratedFromForIn*>(anno)))
                .Union());
    };

    // IsAutoEnvClass
    annoHandler[typeid(CHIR::IsAutoEnvClass)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_isAutoEnvClass);
        annos.emplace_back(PackageFormat::CreateIsAutoEnvClass(
            builder, IsAutoEnvClass::Extract(StaticCast<CHIR::IsAutoEnvClass*>(anno)))
                .Union());
    };

    // IsCapturedClassInCC
    annoHandler[typeid(CHIR::IsCapturedClassInCC)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_isCapturedClassInCC);
        annos.emplace_back(PackageFormat::CreateIsCapturedClassInCC(
            builder, IsCapturedClassInCC::Extract(StaticCast<CHIR::IsCapturedClassInCC*>(anno)))
                .Union());
    };

    // EnumCaseIndex
    annoHandler[typeid(CHIR::EnumCaseIndex)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_enumCaseIndex);
        auto index = EnumCaseIndex::Extract(StaticCast<CHIR::EnumCaseIndex*>(anno));
        int64_t indexNum = -1;
        if (index.has_value()) {
            indexNum = static_cast<int64_t>(index.value());
        }
        annos.emplace_back(PackageFormat::CreateEnumCaseIndex(builder, indexNum).Union());
    };

    // VirMethodOffset
    annoHandler[typeid(CHIR::VirMethodOffset)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_virMethodOffset);
        auto offset = VirMethodOffset::Extract(StaticCast<CHIR::VirMethodOffset*>(anno));
        int64_t offsetNum = -1;
        if (offset.has_value()) {
            offsetNum = static_cast<int64_t>(offset.value());
        }
        annos.emplace_back(PackageFormat::CreateVirMethodOffset(builder, offsetNum).Union());
    };

    // OverrideSrcFuncType
    annoHandler[typeid(CHIR::OverrideSrcFuncType)] = [this, &annos, &annoTypes](Annotation* anno) {
        annoTypes.push_back(PackageFormat::Annotation::Annotation_overrideSrcFuncType);
        auto funcType = GetId<Type>(OverrideSrcFuncType::Extract(StaticCast<CHIR::OverrideSrcFuncType*>(anno)));
        annos.emplace_back(PackageFormat::CreateOverrideSrcFuncType(builder, funcType).Union());
    };
    
    annoHandler[typeid(CHIR::AnnoFactoryInfo)] = Empty;

    for (auto& entry : obj.GetAnno().GetAnnos()) {
        if (annoHandler.count(entry.first) != 0) {
            annoHandler.at(entry.first)(entry.second.get());
        } else {
            CJC_ABORT();
        }
    }
    auto loc = Serialize<PackageFormat::DebugLocation>(obj.Base::GetDebugLocation());
    auto attributes = obj.GetAttributeInfo().GetRawAttrs().to_ulong();
    return PackageFormat::CreateBaseDirect(builder, &annoTypes, &annos, loc, attributes);
}

template <>
flatbuffers::Offset<PackageFormat::Expression> CHIRSerializer::CHIRSerializerImpl::Serialize(const Expression& obj);

template <>
flatbuffers::Offset<PackageFormat::MemberVarInfo> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const MemberVarInfo& obj)
{
    auto name = obj.name;
    auto rawMangledName = obj.rawMangledName;
    auto type = GetId<Type>(obj.type);
    auto attributes = obj.attributeInfo.GetRawAttrs().to_ulong();
    auto loc = Serialize<PackageFormat::DebugLocation>(obj.loc);
    auto annoInfo = Serialize<PackageFormat::AnnoInfo>(obj.annoInfo);
    auto initializerFunc = GetId<Value>(obj.initializerFunc);
    auto outerDef = GetId<CustomTypeDef>(obj.outerDef);
    return PackageFormat::CreateMemberVarInfoDirect(
        builder, name.data(), rawMangledName.data(), type, attributes, loc, annoInfo, initializerFunc, outerDef);
}

template <>
flatbuffers::Offset<PackageFormat::EnumCtorInfo> CHIRSerializer::CHIRSerializerImpl::Serialize(const EnumCtorInfo& obj)
{
    return PackageFormat::CreateEnumCtorInfoDirect(
        builder, obj.name.data(), obj.mangledName.data(), GetId<Type>(obj.funcType));
}

// ========================== Type Serializers =================================

template <> flatbuffers::Offset<PackageFormat::Type> CHIRSerializer::CHIRSerializerImpl::Serialize(const Type& obj)
{
    auto kind = PackageFormat::CHIRTypeKind(obj.GetTypeKind());
    auto argTys = GetId<Type>(obj.GetTypeArgs());
    return PackageFormat::CreateTypeDirect(builder, kind, argTys.empty() ? nullptr : &argTys);
}

template <>
flatbuffers::Offset<PackageFormat::RawArrayType> CHIRSerializer::CHIRSerializerImpl::Serialize(const RawArrayType& obj)
{
    auto base = Serialize<PackageFormat::Type>(static_cast<const Type&>(obj));
    auto dims = obj.GetDims();
    return PackageFormat::CreateRawArrayType(builder, base, dims);
}

template <>
flatbuffers::Offset<PackageFormat::VArrayType> CHIRSerializer::CHIRSerializerImpl::Serialize(const VArrayType& obj)
{
    auto base = Serialize<PackageFormat::Type>(static_cast<const Type&>(obj));
    auto size = obj.GetSize();
    return PackageFormat::CreateVArrayType(builder, base, size);
}

template <>
flatbuffers::Offset<PackageFormat::FuncType> CHIRSerializer::CHIRSerializerImpl::Serialize(const FuncType& obj)
{
    auto base = Serialize<PackageFormat::Type>(static_cast<const Type&>(obj));
    auto isCFuncType = obj.IsCFunc();
    auto hasVarArg = obj.HasVarArg();
    return PackageFormat::CreateFuncType(builder, base, isCFuncType, hasVarArg);
}

template <>
flatbuffers::Offset<PackageFormat::CustomType> CHIRSerializer::CHIRSerializerImpl::Serialize(const CustomType& obj)
{
    auto base = Serialize<PackageFormat::Type>(static_cast<const Type&>(obj));
    auto customTypeDef = GetId<CustomTypeDef>(obj.GetCustomTypeDef());
    return PackageFormat::CreateCustomType(builder, base, customTypeDef);
}

template <>
flatbuffers::Offset<PackageFormat::GenericType> CHIRSerializer::CHIRSerializerImpl::Serialize(const GenericType& obj)
{
    auto base = Serialize<PackageFormat::Type>(static_cast<const Type&>(obj));
    auto identifier = obj.GetIdentifier();
    auto srcCodeIndentifier = obj.GetSrcCodeIdentifier();
    auto upperBounds = GetId<Type>(obj.GetUpperBounds());
    return PackageFormat::CreateGenericTypeDirect(builder, base, identifier.data(),
        srcCodeIndentifier.data(), upperBounds.empty() ? nullptr : &upperBounds);
}

// ======================= Value Serializers ===================================
template <> flatbuffers::Offset<PackageFormat::Value> CHIRSerializer::CHIRSerializerImpl::Serialize(const Value& obj)
{
    auto base = Serialize<PackageFormat::Base>(static_cast<const Base&>(obj));
    auto identifier = obj.GetIdentifier();
    auto type = GetId<Type>(obj.GetType());
    // Map C++ ValueKind to schema ValueKind (IMPORTED_* removed; use attrs for imported)
    PackageFormat::ValueKind kind;
    switch (obj.GetValueKind()) {
        case Value::ValueKind::KIND_LITERAL:
            kind = PackageFormat::ValueKind_LITERAL;
            break;
        case Value::ValueKind::KIND_GLOBALVAR:
            kind = PackageFormat::ValueKind_GLOBALVAR;
            break;
        case Value::ValueKind::KIND_PARAMETER:
            kind = PackageFormat::ValueKind_PARAMETER;
            break;
        case Value::ValueKind::KIND_LOCALVAR:
            kind = PackageFormat::ValueKind_LOCALVAR;
            break;
        case Value::ValueKind::KIND_FUNC:
            kind = PackageFormat::ValueKind_FUNC;
            break;
        case Value::ValueKind::KIND_BLOCK:
            kind = PackageFormat::ValueKind_BLOCK;
            break;
        case Value::ValueKind::KIND_BLOCK_GROUP:
            kind = PackageFormat::ValueKind_BLOCK_GROUP;
            break;
        default:
            CJC_ABORT();
    }

    return PackageFormat::CreateValueDirect(builder, base, type, identifier.data(), kind);
}

template <>
flatbuffers::Offset<PackageFormat::Parameter> CHIRSerializer::CHIRSerializerImpl::Serialize(const Parameter& obj)
{
    auto base = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto ownedFunc = GetId<Value>(obj.GetOwnerFunc());
    auto ownedLambda = GetId<Expression>(obj.GetOwnerLambda());
    auto srcCodeIdentifier = obj.GetSrcCodeIdentifier();
    auto annoInfo = Serialize<PackageFormat::AnnoInfo>(obj.GetAnnoInfo());
    return PackageFormat::CreateParameterDirect(
        builder, base, ownedFunc, ownedLambda, srcCodeIdentifier.data(), annoInfo);
}

template <>
flatbuffers::Offset<PackageFormat::LocalVar> CHIRSerializer::CHIRSerializerImpl::Serialize(const LocalVar& obj)
{
    auto base = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto associatedExpr = GetId<Expression>(obj.GetExpr());
    auto isRetVal = obj.IsRetValue();
    auto srcCodeIdentifier = obj.GetSrcCodeIdentifier();
    return PackageFormat::CreateLocalVarDirect(builder, base, associatedExpr, isRetVal, srcCodeIdentifier.data());
}

template <>
flatbuffers::Offset<PackageFormat::GlobalValue> CHIRSerializer::CHIRSerializerImpl::Serialize(const GlobalValue& obj)
{
    auto valueOffset = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto declaredParent = obj.GetParentCustomTypeDef() ? GetId<CustomTypeDef>(obj.GetParentCustomTypeDef()) : 0u;
    std::vector<flatbuffers::Offset<flatbuffers::String>> features;
    for (const auto& name : obj.GetFeatures()) {
        features.push_back(builder.CreateSharedString(name));
    }
    auto annoInfo = Serialize<PackageFormat::AnnoInfo>(obj.GetAnnoInfo());
    return PackageFormat::CreateGlobalValueDirect(builder, valueOffset,
        obj.GetSrcCodeIdentifier().data(), obj.GetRawMangledName().data(), obj.GetPackageName().data(),
        declaredParent, features.empty() ? nullptr : &features, annoInfo);
}

template <>
flatbuffers::Offset<PackageFormat::GlobalVar> CHIRSerializer::CHIRSerializerImpl::Serialize(const GlobalVar& obj)
{
    auto globalSymbolOffset = Serialize<PackageFormat::GlobalValue>(static_cast<const GlobalValue&>(obj));
    uint32_t initializerId = obj.GetInitializerValue() ? GetId<Value>(obj.GetInitializerValue()) : 0;
    return PackageFormat::CreateGlobalVar(builder, globalSymbolOffset, initializerId);
}

template <> flatbuffers::Offset<PackageFormat::Block> CHIRSerializer::CHIRSerializerImpl::Serialize(const Block& obj)
{
    auto base = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto parentGroup = GetId<Value>(obj.GetParentBlockGroup());
    auto exprs = GetId<Expression>(obj.GetExpressions());
    auto predecessors = GetId<Value>(obj.GetPredecessors());
    auto exceptionCatchList = GetId<Type>(obj.IsLandingPadBlock() ? obj.GetExceptions() : std::vector<ClassType*>());
    return PackageFormat::CreateBlockDirect(builder, base, parentGroup, exprs.empty() ? nullptr : &exprs,
        predecessors.empty() ? nullptr : &predecessors, obj.IsLandingPadBlock(),
        exceptionCatchList.empty() ? nullptr : &exceptionCatchList);
}

template <>
flatbuffers::Offset<PackageFormat::BlockGroup> CHIRSerializer::CHIRSerializerImpl::Serialize(const BlockGroup& obj)
{
    CJC_ASSERT(obj.GetOwnerFunc() || obj.GetOwnerExpression());
    auto base = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto entryBlock = GetId<Value>(obj.GetEntryBlock());
    auto blocks = GetId<Value>(obj.GetBlocks());
    auto ownedFunc = GetId<Value>(obj.GetOwnerFunc());
    auto ownedExpression = GetId<Expression>(obj.GetOwnerExpression());
    return PackageFormat::CreateBlockGroupDirect(
        builder, base, entryBlock, blocks.empty() ? nullptr : &blocks, ownedFunc, ownedExpression);
}

template <>
flatbuffers::Offset<PackageFormat::Function> CHIRSerializer::CHIRSerializerImpl::Serialize(const Function& obj)
{
    auto globalSymbolOffset = Serialize<PackageFormat::GlobalValue>(static_cast<const GlobalValue&>(obj));

    // skip serializing genericDecl when it's imported (no body)
    uint32_t genericDecl = 0;
    if (auto gFunc = obj.GetGenericDecl(); gFunc && gFunc->IsFuncWithBody()) {
        genericDecl = GetId<Value>(gFunc);
    }

    auto funcKind = PackageFormat::FuncKind(obj.GetFuncKind());
    flatbuffers::Offset<PackageFormat::FuncSigInfo> originalLambdaInfoOffset = 0;
    if (obj.GetFuncKind() == LAMBDA && obj.originalLambdaInfo.funcType != nullptr) {
        auto funcName = obj.originalLambdaInfo.funcName;
        auto oriLambdaFuncTy = GetId<Type>(obj.originalLambdaInfo.funcType);
        auto oriLambdaGenericTypeParams = GetId<Type>(obj.originalLambdaInfo.genericTypeParams);
        originalLambdaInfoOffset = PackageFormat::CreateFuncSigInfoDirect(builder, funcName.data(), oriLambdaFuncTy,
            oriLambdaGenericTypeParams.empty() ? nullptr : &oriLambdaGenericTypeParams);
    }
    auto genericTypeParams = GetId<Type>(obj.GetGenericTypeParams());
    auto paramDftValHostFunc = GetId<Value>(obj.GetParamDftValHostFunc());

    uint32_t body = 0;
    std::vector<uint32_t> params = GetId<Value>(obj.GetParams());
    uint32_t retVal = 0;
    auto propLoc = Serialize<PackageFormat::DebugLocation>(obj.GetPropLocation());
    if (obj.IsFuncWithBody()) {
        CJC_NULLPTR_CHECK(obj.GetBody());
        body = GetId<Value>(obj.GetBody());
        retVal = GetId<Value>(obj.GetReturnValue());
    }

    return PackageFormat::CreateFunctionDirect(builder, globalSymbolOffset, genericDecl, funcKind, obj.IsFastNative(),
        obj.IsCFFIWrapper(), originalLambdaInfoOffset, genericTypeParams.empty() ? nullptr : &genericTypeParams,
        paramDftValHostFunc, body, params.empty() ? nullptr : &params, retVal, propLoc, obj.localId, obj.blockId,
        obj.blockGroupId);
}

template <>
flatbuffers::Offset<PackageFormat::LiteralValue> CHIRSerializer::CHIRSerializerImpl::Serialize(const LiteralValue& obj)
{
    auto base = Serialize<PackageFormat::Value>(static_cast<const Value&>(obj));
    auto literalKind = PackageFormat::ConstantValueKind(obj.GetConstantValueKind());
    return PackageFormat::CreateLiteralValue(builder, base, literalKind);
}

// ======================= Literal Value Serializers ===========================
template <>
flatbuffers::Offset<PackageFormat::BoolLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const BoolLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    auto val = obj.GetVal();
    return PackageFormat::CreateBoolLiteral(builder, base, val);
}

template <>
flatbuffers::Offset<PackageFormat::RuneLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const RuneLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    auto val = obj.GetVal();
    return PackageFormat::CreateRuneLiteral(builder, base, val);
}

template <>
flatbuffers::Offset<PackageFormat::StringLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const StringLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    auto val = builder.CreateSharedString(obj.GetVal());
    return PackageFormat::CreateStringLiteral(builder, base, val);
}

template <>
flatbuffers::Offset<PackageFormat::IntLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const IntLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    auto val = obj.GetUnsignedVal();
    return PackageFormat::CreateIntLiteral(builder, base, val);
}

template <>
flatbuffers::Offset<PackageFormat::FloatLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const FloatLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    auto val = obj.GetVal();
    return PackageFormat::CreateFloatLiteral(builder, base, val);
}

template <>
flatbuffers::Offset<PackageFormat::UnitLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const UnitLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    return PackageFormat::CreateUnitLiteral(builder, base);
}

template <>
flatbuffers::Offset<PackageFormat::NullLiteral> CHIRSerializer::CHIRSerializerImpl::Serialize(const NullLiteral& obj)
{
    auto base = Serialize<PackageFormat::LiteralValue>(static_cast<const LiteralValue&>(obj));
    return PackageFormat::CreateNullLiteral(builder, base);
}

// ======================= Expression Serializers ==============================

static PackageFormat::CHIRExprKind ToPackageExprKind(const Expression& expr)
{
    switch (expr.GetExprKind()) {
        case ExprKind::GOTO:                              return PackageFormat::CHIRExprKind_Goto;
        case ExprKind::BRANCH:                            return PackageFormat::CHIRExprKind_Branch;
        case ExprKind::MULTIBRANCH:                       return PackageFormat::CHIRExprKind_MultiBranch;
        case ExprKind::EXIT:                              return PackageFormat::CHIRExprKind_Exit;
        case ExprKind::APPLY_WITH_EXCEPTION:              return PackageFormat::CHIRExprKind_TryApply;
        case ExprKind::INVOKE_WITH_EXCEPTION:             return PackageFormat::CHIRExprKind_TryInvoke;
        case ExprKind::INVOKESTATIC_WITH_EXCEPTION:       return PackageFormat::CHIRExprKind_TryInvoke;
        case ExprKind::RAISE_EXCEPTION:                   return PackageFormat::CHIRExprKind_RaiseException;
        case ExprKind::INT_OP_WITH_EXCEPTION: {
            const auto& intOp = static_cast<const IntOpWithException&>(expr);
            switch (intOp.GetOpKind()) {
                case ExprKind::NEG:    return PackageFormat::CHIRExprKind_TryNeg;
                case ExprKind::ADD:    return PackageFormat::CHIRExprKind_TryAdd;
                case ExprKind::SUB:    return PackageFormat::CHIRExprKind_TrySub;
                case ExprKind::MUL:    return PackageFormat::CHIRExprKind_TryMul;
                case ExprKind::DIV:    return PackageFormat::CHIRExprKind_TryDiv;
                case ExprKind::MOD:    return PackageFormat::CHIRExprKind_TryMod;
                case ExprKind::EXP:    return PackageFormat::CHIRExprKind_TryExp;
                case ExprKind::LSHIFT: return PackageFormat::CHIRExprKind_TryLShift;
                case ExprKind::RSHIFT: return PackageFormat::CHIRExprKind_TryRShift;
                default:               return PackageFormat::CHIRExprKind_Invalid;
            }
        }
        case ExprKind::SPAWN_WITH_EXCEPTION:              return PackageFormat::CHIRExprKind_TrySpawn;
        case ExprKind::TYPECAST_WITH_EXCEPTION:           return PackageFormat::CHIRExprKind_TryNumericCast;
        case ExprKind::INTRINSIC_WITH_EXCEPTION:          return PackageFormat::CHIRExprKind_TryIntrinsic;
        case ExprKind::ALLOCATE_WITH_EXCEPTION:           return PackageFormat::CHIRExprKind_TryAllocate;
        case ExprKind::RAW_ARRAY_ALLOCATE_WITH_EXCEPTION: return PackageFormat::CHIRExprKind_TryRawArrayAllocate;
        case ExprKind::NEG:                               return PackageFormat::CHIRExprKind_Neg;
        case ExprKind::NOT:                               return PackageFormat::CHIRExprKind_Not;
        case ExprKind::BITNOT:                            return PackageFormat::CHIRExprKind_BitNot;
        case ExprKind::ADD:                               return PackageFormat::CHIRExprKind_Add;
        case ExprKind::SUB:                               return PackageFormat::CHIRExprKind_Sub;
        case ExprKind::MUL:                               return PackageFormat::CHIRExprKind_Mul;
        case ExprKind::DIV:                               return PackageFormat::CHIRExprKind_Div;
        case ExprKind::MOD:                               return PackageFormat::CHIRExprKind_Mod;
        case ExprKind::EXP:                               return PackageFormat::CHIRExprKind_Exp;
        case ExprKind::LSHIFT:                            return PackageFormat::CHIRExprKind_LShift;
        case ExprKind::RSHIFT:                            return PackageFormat::CHIRExprKind_RShift;
        case ExprKind::BITAND:                            return PackageFormat::CHIRExprKind_BitAnd;
        case ExprKind::BITOR:                             return PackageFormat::CHIRExprKind_BitOr;
        case ExprKind::BITXOR:                            return PackageFormat::CHIRExprKind_BitXor;
        case ExprKind::LT:                                return PackageFormat::CHIRExprKind_LT;
        case ExprKind::GT:                                return PackageFormat::CHIRExprKind_GT;
        case ExprKind::LE:                                return PackageFormat::CHIRExprKind_LE;
        case ExprKind::GE:                                return PackageFormat::CHIRExprKind_GE;
        case ExprKind::EQUAL:                             return PackageFormat::CHIRExprKind_Equal;
        case ExprKind::NOTEQUAL:                          return PackageFormat::CHIRExprKind_NotEqual;
        case ExprKind::AND:                               return PackageFormat::CHIRExprKind_And;
        case ExprKind::OR:                                return PackageFormat::CHIRExprKind_Or;
        case ExprKind::TYPECAST:                          return PackageFormat::CHIRExprKind_NumericCast;
        case ExprKind::BOX:                               return PackageFormat::CHIRExprKind_Box;
        case ExprKind::UNBOX:                             return PackageFormat::CHIRExprKind_UnboxToValue;
        case ExprKind::UNBOX_TO_REF:                      return PackageFormat::CHIRExprKind_UnboxToRef;
        case ExprKind::TRANSFORM_TO_GENERIC:              return PackageFormat::CHIRExprKind_CastToGeneric;
        case ExprKind::TRANSFORM_TO_CONCRETE:             return PackageFormat::CHIRExprKind_CastToConcrete;
        case ExprKind::ALLOCATE:                          return PackageFormat::CHIRExprKind_Allocate;
        case ExprKind::LOAD:                              return PackageFormat::CHIRExprKind_Load;
        case ExprKind::STORE:                             return PackageFormat::CHIRExprKind_Store;
        case ExprKind::GET_ELEMENT_BY_NAME:               return PackageFormat::CHIRExprKind_GetElementByName;
        case ExprKind::GET_ELEMENT_REF:                   return PackageFormat::CHIRExprKind_GetElementRef;
        case ExprKind::STORE_ELEMENT_BY_NAME:             return PackageFormat::CHIRExprKind_StoreElementByName;
        case ExprKind::STORE_ELEMENT_REF:                 return PackageFormat::CHIRExprKind_StoreElementRef;
        case ExprKind::FIELD:                             return PackageFormat::CHIRExprKind_Field;
        case ExprKind::FIELD_BY_NAME:                     return PackageFormat::CHIRExprKind_FieldByName;
        case ExprKind::RAW_ARRAY_ALLOCATE:                return PackageFormat::CHIRExprKind_RawArrayAllocate;
        case ExprKind::RAW_ARRAY_LITERAL_INIT:            return PackageFormat::CHIRExprKind_RawArrayLiteralInit;
        case ExprKind::RAW_ARRAY_INIT_BY_VALUE:           return PackageFormat::CHIRExprKind_RawArrayInitByValue;
        case ExprKind::VARRAY:                            return PackageFormat::CHIRExprKind_VArrayExpr;
        case ExprKind::VARRAY_BUILDER:                    return PackageFormat::CHIRExprKind_VArrayBuilder;
        case ExprKind::CONSTANT:                          return PackageFormat::CHIRExprKind_Constant;
        case ExprKind::DEBUGEXPR:                         return PackageFormat::CHIRExprKind_Debug;
        case ExprKind::TUPLE:                             return PackageFormat::CHIRExprKind_Tuple;
        case ExprKind::INSTANCEOF:                        return PackageFormat::CHIRExprKind_InstanceOf;
        case ExprKind::GET_EXCEPTION:                     return PackageFormat::CHIRExprKind_GetException;
        case ExprKind::SPAWN:                             return PackageFormat::CHIRExprKind_Spawn;
        case ExprKind::LAMBDA:                            return PackageFormat::CHIRExprKind_Lambda;
        case ExprKind::GET_INSTANTIATE_VALUE:             return PackageFormat::CHIRExprKind_GetInstantiateValue;
        case ExprKind::APPLY:                             return PackageFormat::CHIRExprKind_Apply;
        case ExprKind::INVOKE:                            return PackageFormat::CHIRExprKind_Invoke;
        case ExprKind::INVOKESTATIC:                      return PackageFormat::CHIRExprKind_Invoke;
        case ExprKind::INTRINSIC:                         return PackageFormat::CHIRExprKind_Intrinsic;
        case ExprKind::GET_RTTI:                          return PackageFormat::CHIRExprKind_GetRtti;
        case ExprKind::GET_RTTI_STATIC:                   return PackageFormat::CHIRExprKind_GetRttiStatic;
        case ExprKind::FORIN_RANGE:
        case ExprKind::FORIN_ITER:
        case ExprKind::FORIN_CLOSED_RANGE:
        case ExprKind::INVALID:
        case ExprKind::MAX_EXPR_KINDS:
        default:                                          return PackageFormat::CHIRExprKind_Invalid;
    }
}

template <>
flatbuffers::Offset<PackageFormat::Expression> CHIRSerializer::CHIRSerializerImpl::Serialize(const Expression& obj)
{
    auto base = Serialize<PackageFormat::Base>(static_cast<const Base&>(obj));
    auto kind = ToPackageExprKind(obj);
    auto operands = GetId<Value>(obj.Expression::GetOperands());
    auto blockGroups = GetId<Value>(obj.GetBlockGroups());
    auto owner = GetId<Value>(obj.GetParentBlock());
    auto resultLocalVar = GetId<Value>(obj.GetResult());
    auto resultTy = GetId<Type>(obj.GetResult() ? obj.GetResult()->GetType() : nullptr);
    return PackageFormat::CreateExpressionDirect(builder, base, kind,
        operands.empty() ? nullptr : &operands, blockGroups.empty() ? nullptr : &blockGroups, owner,
        resultLocalVar, resultTy);
}

template <>
flatbuffers::Offset<PackageFormat::UnaryExpressionBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const UnaryExpression& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateUnaryExpressionBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::UnaryExpressionBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const IntOpWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateUnaryExpressionBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::BinaryExpressionBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const BinaryExpression& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateBinaryExpressionBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::BinaryExpressionBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const IntOpWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateBinaryExpressionBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::AllocateBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const Allocate& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto allocatedType = GetId<Type>(obj.GetType());
    return PackageFormat::CreateAllocateBase(builder, base, allocatedType);
}

template <>
flatbuffers::Offset<PackageFormat::AllocateBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const AllocateWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto allocatedType = GetId<Type>(obj.GetType());
    return PackageFormat::CreateAllocateBase(builder, base, allocatedType);
}

template <>
flatbuffers::Offset<PackageFormat::GetElementRef> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const GetElementRef& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto path = obj.GetPath();
    return PackageFormat::CreateGetElementRefDirect(builder, base, path.empty() ? nullptr : &path);
}

template <>
flatbuffers::Offset<PackageFormat::GetElementByName> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const GetElementByName& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto names = builder.CreateVectorOfStrings(obj.GetNames());
    return PackageFormat::CreateGetElementByName(builder, base, names);
}

template <>
flatbuffers::Offset<PackageFormat::StoreElementRef> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const StoreElementRef& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto path = obj.GetPath();
    return PackageFormat::CreateStoreElementRefDirect(builder, base, &path);
}

template <>
flatbuffers::Offset<PackageFormat::StoreElementByName> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const StoreElementByName& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto names = builder.CreateVectorOfStrings(obj.GetNames());
    return PackageFormat::CreateStoreElementByName(builder, base, names);
}

template <>
flatbuffers::Offset<PackageFormat::ApplyBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const Apply& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize Apply
    return PackageFormat::CreateApplyBase(builder, funcCall, obj.IsSuperCall());
}

template <>
flatbuffers::Offset<PackageFormat::ApplyBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const ApplyWithException& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize ApplyWithException
    return PackageFormat::CreateApplyBase(builder, funcCall, false);
}

template <>
flatbuffers::Offset<PackageFormat::FuncSigInfo> CHIRSerializer::CHIRSerializerImpl::Serialize(const FuncSigInfo& obj)
{
    auto tempTypes = GetId<Type>(obj.genericTypeParams);
    return PackageFormat::CreateFuncSigInfoDirect(builder, obj.funcName.data(),
        GetId<Type>(obj.funcType), tempTypes.empty() ? nullptr : &tempTypes);
}

template <>
flatbuffers::Offset<PackageFormat::InvokeBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const Invoke& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize virMethodCtx
    auto virMethodCtx = Serialize<PackageFormat::FuncSigInfo>(obj.virMethodCtx);
    
    // 4. serialize Invoke
    return PackageFormat::CreateInvokeBase(builder, funcCall, virMethodCtx);
}

template <>
flatbuffers::Offset<PackageFormat::InvokeBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const InvokeWithException& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize virMethodCtx
    auto virMethodCtx = Serialize<PackageFormat::FuncSigInfo>(obj.virMethodCtx);
    
    // 4. serialize InvokeWithException
    return PackageFormat::CreateInvokeBase(builder, funcCall, virMethodCtx);
}

template <>
flatbuffers::Offset<PackageFormat::InvokeBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const InvokeStatic& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize virMethodCtx
    auto virMethodCtx = Serialize<PackageFormat::FuncSigInfo>(obj.virMethodCtx);
    
    // 4. serialize InvokeStatic
    return PackageFormat::CreateInvokeBase(builder, funcCall, virMethodCtx);
}

template <>
flatbuffers::Offset<PackageFormat::InvokeBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const InvokeStaticWithException& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto thisType = GetId<Type>(obj.GetThisType());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instTypeArgs.empty() ? nullptr : &instTypeArgs, thisType);

    // 3. serialize virMethodCtx
    auto virMethodCtx = Serialize<PackageFormat::FuncSigInfo>(obj.virMethodCtx);
    
    // 4. serialize InvokeStaticWithException
    return PackageFormat::CreateInvokeBase(builder, funcCall, virMethodCtx);
}

template <>
flatbuffers::Offset<PackageFormat::NumericCastBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const TypeCast& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateNumericCastBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::NumericCastBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const TypeCastWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto overflowStrategy = PackageFormat::OverflowStrategy(obj.GetOverflowStrategy());
    return PackageFormat::CreateNumericCastBase(builder, base, overflowStrategy);
}

template <>
flatbuffers::Offset<PackageFormat::InstanceOf> CHIRSerializer::CHIRSerializerImpl::Serialize(const InstanceOf& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto targetType = GetId<Type>(obj.GetType());
    return PackageFormat::CreateInstanceOf(builder, base, targetType);
}

template <>
flatbuffers::Offset<PackageFormat::Branch> CHIRSerializer::CHIRSerializerImpl::Serialize(const Branch& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto sourceExpr = obj.GetSourceExpr();
    return PackageFormat::CreateBranch(builder, base, PackageFormat::SourceExpr(sourceExpr));
}

template <>
flatbuffers::Offset<PackageFormat::MultiBranch> CHIRSerializer::CHIRSerializerImpl::Serialize(const MultiBranch& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto caseVals = obj.GetCaseVals();
    return PackageFormat::CreateMultiBranchDirect(builder, base, caseVals.empty() ? nullptr : &caseVals);
}

template <>
flatbuffers::Offset<PackageFormat::GetInstantiateValue> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const GetInstantiateValue& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto instantiateTys = GetId<Type>(obj.GetInstantiateTypes());
    return PackageFormat::CreateGetInstantiateValueDirect(
        builder, base, instantiateTys.empty() ? nullptr : &instantiateTys);
}

template <>
flatbuffers::Offset<PackageFormat::GetRTTIStatic> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const GetRTTIStatic& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto rtti = GetId<Type>(obj.GetRTTIType());
    return PackageFormat::CreateGetRTTIStatic(builder, base, rtti);
}

template <>
flatbuffers::Offset<PackageFormat::Field> CHIRSerializer::CHIRSerializerImpl::Serialize(const Field& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto path = obj.GetPath();
    return PackageFormat::CreateFieldDirect(builder, base, path.empty() ? nullptr : &path);
}

template <>
flatbuffers::Offset<PackageFormat::FieldByName> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const FieldByName& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto names = builder.CreateVectorOfStrings(obj.GetNames());
    return PackageFormat::CreateFieldByName(builder, base, names);
}

template <>
flatbuffers::Offset<PackageFormat::RawArrayAllocateBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const RawArrayAllocate& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto elementType = GetId<Type>(obj.GetElementType());
    return PackageFormat::CreateRawArrayAllocateBase(builder, base, elementType);
}

template <>
flatbuffers::Offset<PackageFormat::RawArrayAllocateBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const RawArrayAllocateWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto elementType = GetId<Type>(obj.GetElementType());
    return PackageFormat::CreateRawArrayAllocateBase(builder, base, elementType);
}

template <>
flatbuffers::Offset<PackageFormat::IntrinsicBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const Intrinsic& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instantiatedTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instantiatedTypeArgs.empty() ? nullptr : &instantiatedTypeArgs, 0);

    // 3. serialize Intrinsic
    auto intrinsicKind = PackageFormat::IntrinsicKind(obj.GetIntrinsicKind());
    return PackageFormat::CreateIntrinsicBase(builder, funcCall, intrinsicKind);
}

template <>
flatbuffers::Offset<PackageFormat::IntrinsicBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const IntrinsicWithException& obj)
{
    // 1. serialize Expression
    auto exprBase = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));

    // 2. serialize FuncCall
    auto instantiatedTypeArgs = GetId<Type>(obj.GetInstantiatedTypeArgs());
    auto funcCall = PackageFormat::CreateFuncCallDirect(
        builder, exprBase, instantiatedTypeArgs.empty() ? nullptr : &instantiatedTypeArgs, 0);

    // 3. serialize IntrinsicWithException
    auto intrinsicKind = PackageFormat::IntrinsicKind(obj.GetIntrinsicKind());
    return PackageFormat::CreateIntrinsicBase(builder, funcCall, intrinsicKind);
}

template <>
flatbuffers::Offset<PackageFormat::Debug> CHIRSerializer::CHIRSerializerImpl::Serialize(const Debug& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto srcCodeIdentifier = obj.GetSrcCodeIdentifier();
    CJC_ASSERT(!obj.GetOperands().empty());
    return PackageFormat::CreateDebugDirect(builder, base, srcCodeIdentifier.data());
}

template <>
flatbuffers::Offset<PackageFormat::SpawnBase> CHIRSerializer::CHIRSerializerImpl::Serialize(const Spawn& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto executeClosure = GetId<Value>(obj.GetExecuteClosure());
    return PackageFormat::CreateSpawnBase(builder, base, executeClosure);
}

template <>
flatbuffers::Offset<PackageFormat::SpawnBase> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const SpawnWithException& obj)
{
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto executeClosure = GetId<Value>(obj.GetExecuteClosure());
    return PackageFormat::CreateSpawnBase(builder, base, executeClosure);
}

template <>
flatbuffers::Offset<PackageFormat::Lambda> CHIRSerializer::CHIRSerializerImpl::Serialize(const Lambda& obj)
{
    CJC_ASSERT(obj.GetBlockGroups().size() == 1);
    CJC_ASSERT(obj.GetBody());
    auto base = Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj));
    auto funcTy = GetId<Type>(obj.GetFuncType());
    auto isLocalFunc = obj.IsLocalFunc();
    auto identifier = obj.GetIdentifier();
    auto srcCodeIdentifier = obj.GetSrcCodeIdentifier();
    auto params = GetId<Value>(obj.GetParams());
    auto genericTypeParams = GetId<Type>(obj.GetGenericTypeParams());
    auto body = GetId<Value>(obj.GetBody());
    auto retVal = GetId<Value>(obj.GetReturnValue());
    auto isConst = obj.IsCompileTimeValue();
    return PackageFormat::CreateLambdaDirect(builder, base, funcTy, isLocalFunc, identifier.data(),
        srcCodeIdentifier.data(), params.empty() ? nullptr : &params,
        genericTypeParams.empty() ? nullptr : &genericTypeParams, body, retVal, isConst);
}

// ======================= Custom Type Def Serializers =========================
template <>
flatbuffers::Offset<PackageFormat::VirtualMethodInfo> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const VirtualMethodInfo& obj)
{
    // condition
    std::string funcName = obj.GetMethodName();
    auto sigType = GetId<Type>(obj.GetMethodSigType());
    auto genericTypeParams = GetId<Type>(obj.GetGenericTypeParams());
    // result
    uint32_t funcPtr = GetId<Value>(obj.GetVirtualMethod());
    auto attributes = obj.GetAttributeInfo().GetRawAttrs().to_ulong();
    auto originalType = GetId<Type>(obj.GetOriginalFuncType());
    auto parentType = GetId<Type>(obj.GetInstParentType());
    auto returnType = GetId<Type>(obj.GetMethodInstRetType());

    return PackageFormat::CreateVirtualMethodInfoDirect(builder, funcName.data(), sigType,
        genericTypeParams.empty() ? nullptr : &genericTypeParams, funcPtr, attributes, originalType, parentType,
        returnType);
}

std::vector<flatbuffers::Offset<PackageFormat::VTableInType>> CHIRSerializer::CHIRSerializerImpl::SerializeVTable(
    const VTableInDef& obj)
{
    std::vector<flatbuffers::Offset<PackageFormat::VTableInType>> retval;
    for (const auto& elem : obj.GetTypeVTables()) {
        auto ty = GetId<Type>(elem.GetSrcParentType());
        auto info = SerializeVec<PackageFormat::VirtualMethodInfo>(elem.GetVirtualMethods());
        retval.push_back(PackageFormat::CreateVTableInTypeDirect(builder, ty, &info));
    }
    return retval;
}

template <>
flatbuffers::Offset<PackageFormat::CustomTypeDef> CHIRSerializer::CHIRSerializerImpl::Serialize(
    const CustomTypeDef& obj)
{
    auto base = Serialize<PackageFormat::Base>(static_cast<const Base&>(obj));
    auto kind = PackageFormat::CustomDefKind(obj.GetCustomKind());
    auto customTypeDefID = GetId<CustomTypeDef>(&obj);
    auto srcCodeIdentifier = obj.GetSrcCodeIdentifier();
    auto identifier = obj.GetIdentifier();
    auto packageName = obj.GetPackageName();
    auto type = GetId<Type>(obj.CustomTypeDef::GetType());
    auto genericDecl = GetId<CustomTypeDef>(obj.GetGenericDecl());
    auto methods = GetId<Value>(obj.GetMethods());
    auto implementedInterfaces = GetId<Type>(obj.GetImplementedInterfaceTys());
    auto instanceMemberVars = obj.GetCustomKind() == CustomDefKind::TYPE_CLASS
        ? SerializeVec<PackageFormat::MemberVarInfo>(StaticCast<const ClassDef&>(obj).GetDirectInstanceVars())
        : SerializeVec<PackageFormat::MemberVarInfo>(obj.GetAllInstanceVars());
    auto staticMemberVars = GetId<Value>(obj.GetStaticMemberVars());
    auto annoInfo = Serialize<PackageFormat::AnnoInfo>(obj.GetAnnoInfo());
    auto vtable = SerializeVTable(obj.GetDefVTable());
    auto varInitializationFunc = GetId<Value>(obj.GetVarInitializationFunc());
    return PackageFormat::CreateCustomTypeDefDirect(builder, base, kind, customTypeDefID, srcCodeIdentifier.data(),
        identifier.data(), packageName.data(), type, genericDecl, methods.empty() ? nullptr : &methods,
        implementedInterfaces.empty() ? nullptr : &implementedInterfaces, &instanceMemberVars,
        staticMemberVars.empty() ? nullptr : &staticMemberVars, annoInfo, &vtable, varInitializationFunc);
}

template <>
flatbuffers::Offset<PackageFormat::EnumDef> CHIRSerializer::CHIRSerializerImpl::Serialize(const EnumDef& obj)
{
    auto base = Serialize<PackageFormat::CustomTypeDef>(static_cast<const CustomTypeDef&>(obj));
    auto ctors = SerializeVec<PackageFormat::EnumCtorInfo>(obj.GetCtors());
    auto nonExhaustive = !obj.IsExhaustive();
    return PackageFormat::CreateEnumDefDirect(builder, base, &ctors, nonExhaustive);
}

template <>
flatbuffers::Offset<PackageFormat::StructDef> CHIRSerializer::CHIRSerializerImpl::Serialize(const StructDef& obj)
{
    auto base = Serialize<PackageFormat::CustomTypeDef>(static_cast<const CustomTypeDef&>(obj));
    auto isCStruct = obj.IsCStruct();
    return PackageFormat::CreateStructDef(builder, base, isCStruct);
}

template <>
flatbuffers::Offset<PackageFormat::ClassDef> CHIRSerializer::CHIRSerializerImpl::Serialize(const ClassDef& obj)
{
    auto base = Serialize<PackageFormat::CustomTypeDef>(static_cast<const CustomTypeDef&>(obj));
    std::vector<uint32_t> annotationTargetIds;
    const std::vector<uint32_t>* annotationTargetsVec = nullptr;
    if (obj.IsAnnotation()) {
        annotationTargetIds = GetId<Value>(obj.GetAnnotationTargets());
        annotationTargetsVec = &annotationTargetIds;
    }
    auto superClass = GetId<Type>(obj.GetSuperClassTy());
    return PackageFormat::CreateClassDefDirect(
        builder, base, obj.IsClass(), obj.IsAnnotation(), annotationTargetsVec, superClass);
}

template <>
flatbuffers::Offset<PackageFormat::ExtendDef> CHIRSerializer::CHIRSerializerImpl::Serialize(const ExtendDef& obj)
{
    auto base = Serialize<PackageFormat::CustomTypeDef>(static_cast<const CustomTypeDef&>(obj));
    auto extendedType = GetId<Type>(obj.GetExtendedType());
    auto genericParams = GetId<Type>(obj.GetGenericTypeParams());
    return PackageFormat::CreateExtendDefDirect(
        builder, base, extendedType, genericParams.empty() ? nullptr : &genericParams);
}

// ========================== Dispatchers ===============================

template <> flatbuffers::Offset<void> CHIRSerializer::CHIRSerializerImpl::Dispatch(const Type& obj)
{
    switch (obj.GetTypeKind()) {
        case Type::TypeKind::TYPE_INT8:
        case Type::TypeKind::TYPE_INT16:
        case Type::TypeKind::TYPE_INT32:
        case Type::TypeKind::TYPE_INT64:
        case Type::TypeKind::TYPE_INT_NATIVE:
        case Type::TypeKind::TYPE_UINT8:
        case Type::TypeKind::TYPE_UINT16:
        case Type::TypeKind::TYPE_UINT32:
        case Type::TypeKind::TYPE_UINT64:
        case Type::TypeKind::TYPE_UINT_NATIVE:
        case Type::TypeKind::TYPE_FLOAT16:
        case Type::TypeKind::TYPE_FLOAT32:
        case Type::TypeKind::TYPE_FLOAT64:
        case Type::TypeKind::TYPE_RUNE:
        case Type::TypeKind::TYPE_BOOLEAN:
        case Type::TypeKind::TYPE_UNIT:
        case Type::TypeKind::TYPE_NOTHING:
        case Type::TypeKind::TYPE_VOID:
        case Type::TypeKind::TYPE_TUPLE:
        case Type::TypeKind::TYPE_CPOINTER:
        case Type::TypeKind::TYPE_CSTRING:
        case Type::TypeKind::TYPE_REFTYPE:
        case Type::TypeKind::TYPE_BOXTYPE:
        case Type::TypeKind::TYPE_THIS:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_Type);
            return Serialize<PackageFormat::Type>(static_cast<const Type&>(obj)).Union();
        case Type::TypeKind::TYPE_STRUCT:
        case Type::TypeKind::TYPE_ENUM:
        case Type::TypeKind::TYPE_CLASS:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_CustomType);
            return Serialize<PackageFormat::CustomType>(static_cast<const CustomType&>(obj)).Union();
        case Type::TypeKind::TYPE_FUNC:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_FuncType);
            return Serialize<PackageFormat::FuncType>(static_cast<const FuncType&>(obj)).Union();
        case Type::TypeKind::TYPE_RAWARRAY:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_RawArrayType);
            return Serialize<PackageFormat::RawArrayType>(static_cast<const RawArrayType&>(obj)).Union();
        case Type::TypeKind::TYPE_VARRAY:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_VArrayType);
            return Serialize<PackageFormat::VArrayType>(static_cast<const VArrayType&>(obj)).Union();
        case Type::TypeKind::TYPE_GENERIC:
            typeKind[GetId<Type>(&obj) - 1] = static_cast<uint8_t>(PackageFormat::TypeElem_GenericType);
            return Serialize<PackageFormat::GenericType>(static_cast<const GenericType&>(obj)).Union();
        default:
            CJC_ABORT();
            return 0;
    }
}

template <> flatbuffers::Offset<void> CHIRSerializer::CHIRSerializerImpl::Dispatch(const LiteralValue& obj)
{
    switch (obj.GetConstantValueKind()) {
        case ConstantValueKind::KIND_BOOL:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_BoolLiteral;
            return Serialize<PackageFormat::BoolLiteral>(static_cast<const BoolLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_RUNE:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_RuneLiteral;
            return Serialize<PackageFormat::RuneLiteral>(static_cast<const RuneLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_INT:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_IntLiteral;
            return Serialize<PackageFormat::IntLiteral>(static_cast<const IntLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_FLOAT:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_FloatLiteral;
            return Serialize<PackageFormat::FloatLiteral>(static_cast<const FloatLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_STRING:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_StringLiteral;
            return Serialize<PackageFormat::StringLiteral>(static_cast<const StringLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_UNIT:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_UnitLiteral;
            return Serialize<PackageFormat::UnitLiteral>(static_cast<const UnitLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_NULL:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_NullLiteral;
            return Serialize<PackageFormat::NullLiteral>(static_cast<const NullLiteral&>(obj)).Union();
        case ConstantValueKind::KIND_FUNC:
            return 0;
        default:
            CJC_ABORT();
            return 0;
    }
}

template <> flatbuffers::Offset<void> CHIRSerializer::CHIRSerializerImpl::Dispatch(const Value& obj)
{
    switch (obj.GetValueKind()) {
        case Value::ValueKind::KIND_LITERAL:
            return Dispatch<LiteralValue>(static_cast<const LiteralValue&>(obj)).Union();
        case Value::ValueKind::KIND_GLOBALVAR:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_GlobalVar;
            return Serialize<PackageFormat::GlobalVar>(dynamic_cast<const GlobalVar&>(obj)).Union();
        case Value::ValueKind::KIND_PARAMETER:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_Parameter;
            return Serialize<PackageFormat::Parameter>(static_cast<const Parameter&>(obj)).Union();
        case Value::ValueKind::KIND_LOCALVAR:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_LocalVar;
            return Serialize<PackageFormat::LocalVar>(static_cast<const LocalVar&>(obj)).Union();
        case Value::ValueKind::KIND_FUNC:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_Function;
            return Serialize<PackageFormat::Function>(dynamic_cast<const Function&>(obj)).Union();
        case Value::ValueKind::KIND_BLOCK:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_Block;
            return Serialize<PackageFormat::Block>(static_cast<const Block&>(obj)).Union();
        case Value::ValueKind::KIND_BLOCK_GROUP:
            valueKind[GetId<Value>(&obj) - 1] = PackageFormat::ValueElem_BlockGroup;
            return Serialize<PackageFormat::BlockGroup>(static_cast<const BlockGroup&>(obj)).Union();
        default:
            CJC_ABORT();
            return 0;
    }
}

template <> flatbuffers::Offset<void> CHIRSerializer::CHIRSerializerImpl::Dispatch(const Expression& obj)
{
    switch (obj.GetExprKind()) {
        case ExprKind::ALLOCATE:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_AllocateBase;
            return Serialize<PackageFormat::AllocateBase>(static_cast<const Allocate&>(obj)).Union();
        case ExprKind::ALLOCATE_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_AllocateBase;
            return Serialize<PackageFormat::AllocateBase>(static_cast<const AllocateWithException&>(obj)).Union();
        case ExprKind::APPLY:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_ApplyBase;
            return Serialize<PackageFormat::ApplyBase>(static_cast<const Apply&>(obj)).Union();
        case ExprKind::APPLY_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_ApplyBase;
            return Serialize<PackageFormat::ApplyBase>(static_cast<const ApplyWithException&>(obj)).Union();
        case ExprKind::ADD:
        case ExprKind::SUB:
        case ExprKind::MUL:
        case ExprKind::DIV:
        case ExprKind::MOD:
        case ExprKind::EXP:
        case ExprKind::LSHIFT:
        case ExprKind::RSHIFT:
        case ExprKind::BITAND:
        case ExprKind::BITOR:
        case ExprKind::BITXOR:
        case ExprKind::LT:
        case ExprKind::GT:
        case ExprKind::LE:
        case ExprKind::GE:
        case ExprKind::EQUAL:
        case ExprKind::NOTEQUAL:
        case ExprKind::AND:
        case ExprKind::OR:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_BinaryExpressionBase;
            return Serialize<PackageFormat::BinaryExpressionBase>(static_cast<const BinaryExpression&>(obj)).Union();
        case ExprKind::BRANCH:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_Branch;
            return Serialize<PackageFormat::Branch>(static_cast<const Branch&>(obj)).Union();
        case ExprKind::DEBUGEXPR:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_Debug;
            return Serialize<PackageFormat::Debug>(static_cast<const Debug&>(obj)).Union();
        case ExprKind::FIELD:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_Field;
            return Serialize<PackageFormat::Field>(static_cast<const Field&>(obj)).Union();
        case ExprKind::FIELD_BY_NAME:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_FieldByName;
            return Serialize<PackageFormat::FieldByName>(static_cast<const FieldByName&>(obj)).Union();
        case ExprKind::GET_ELEMENT_BY_NAME:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_GetElementByName;
            return Serialize<PackageFormat::GetElementByName>(static_cast<const GetElementByName&>(obj)).Union();
        case ExprKind::GET_ELEMENT_REF:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_GetElementRef;
            return Serialize<PackageFormat::GetElementRef>(static_cast<const GetElementRef&>(obj)).Union();
        case ExprKind::GET_INSTANTIATE_VALUE:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_GetInstantiateValue;
            return Serialize<PackageFormat::GetInstantiateValue>(static_cast<const GetInstantiateValue&>(obj)).Union();
        case ExprKind::GET_RTTI_STATIC:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_GetRTTIStatic;
            return Serialize<PackageFormat::GetRTTIStatic>(static_cast<const GetRTTIStatic&>(obj)).Union();
        case ExprKind::INSTANCEOF:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_InstanceOf;
            return Serialize<PackageFormat::InstanceOf>(static_cast<const InstanceOf&>(obj)).Union();
        case ExprKind::INT_OP_WITH_EXCEPTION: {
            const auto& expr = static_cast<const IntOpWithException&>(obj);
            if (ExprKindMgr::Instance()->GetMajorKind(expr.GetOpKind()) == ExprMajorKind::UNARY_EXPR) {
                exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_UnaryExpressionBase;
                return Serialize<PackageFormat::UnaryExpressionBase>(expr).Union();
            } else {
                exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_BinaryExpressionBase;
                return Serialize<PackageFormat::BinaryExpressionBase>(expr).Union();
            }
        }
        case ExprKind::INTRINSIC:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_IntrinsicBase;
            return Serialize<PackageFormat::IntrinsicBase>(static_cast<const Intrinsic&>(obj)).Union();
        case ExprKind::INTRINSIC_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_IntrinsicBase;
            return Serialize<PackageFormat::IntrinsicBase>(static_cast<const IntrinsicWithException&>(obj)).Union();
        case ExprKind::INVOKE:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_InvokeBase;
            return Serialize<PackageFormat::InvokeBase>(static_cast<const Invoke&>(obj)).Union();
        case ExprKind::INVOKE_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_InvokeBase;
            return Serialize<PackageFormat::InvokeBase>(static_cast<const InvokeWithException&>(obj)).Union();
        case ExprKind::INVOKESTATIC:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_InvokeBase;
            return Serialize<PackageFormat::InvokeBase>(static_cast<const InvokeStatic&>(obj)).Union();
        case ExprKind::INVOKESTATIC_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_InvokeBase;
            return Serialize<PackageFormat::InvokeBase>(static_cast<const InvokeStaticWithException&>(obj)).Union();
        case ExprKind::LAMBDA:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_Lambda;
            return Serialize<PackageFormat::Lambda>(static_cast<const Lambda&>(obj)).Union();
        case ExprKind::MULTIBRANCH:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_MultiBranch;
            return Serialize<PackageFormat::MultiBranch>(static_cast<const MultiBranch&>(obj)).Union();
        case ExprKind::TYPECAST:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_NumericCastBase;
            return Serialize<PackageFormat::NumericCastBase>(static_cast<const TypeCast&>(obj)).Union();
        case ExprKind::TYPECAST_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_NumericCastBase;
            return Serialize<PackageFormat::NumericCastBase>(static_cast<const TypeCastWithException&>(obj)).Union();
        case ExprKind::RAW_ARRAY_ALLOCATE:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_RawArrayAllocateBase;
            return Serialize<PackageFormat::RawArrayAllocateBase>(static_cast<const RawArrayAllocate&>(obj)).Union();
        case ExprKind::RAW_ARRAY_ALLOCATE_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_RawArrayAllocateBase;
            return Serialize<PackageFormat::RawArrayAllocateBase>(
                static_cast<const RawArrayAllocateWithException&>(obj)).Union();
        case ExprKind::SPAWN:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_SpawnBase;
            return Serialize<PackageFormat::SpawnBase>(static_cast<const Spawn&>(obj)).Union();
        case ExprKind::SPAWN_WITH_EXCEPTION:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_SpawnBase;
            return Serialize<PackageFormat::SpawnBase>(static_cast<const SpawnWithException&>(obj)).Union();
        case ExprKind::STORE_ELEMENT_BY_NAME:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_StoreElementByName;
            return Serialize<PackageFormat::StoreElementByName>(static_cast<const StoreElementByName&>(obj)).Union();
        case ExprKind::STORE_ELEMENT_REF:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_StoreElementRef;
            return Serialize<PackageFormat::StoreElementRef>(static_cast<const StoreElementRef&>(obj)).Union();
        case ExprKind::NEG:
        case ExprKind::NOT:
        case ExprKind::BITNOT:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_UnaryExpressionBase;
            return Serialize<PackageFormat::UnaryExpressionBase>(static_cast<const UnaryExpression&>(obj)).Union();
        case ExprKind::GOTO:
        case ExprKind::EXIT:
        case ExprKind::RAISE_EXCEPTION:
        case ExprKind::LOAD:
        case ExprKind::STORE:
        case ExprKind::CONSTANT:
        case ExprKind::FORIN_RANGE:
        case ExprKind::FORIN_ITER:
        case ExprKind::FORIN_CLOSED_RANGE:
        case ExprKind::TUPLE:
        case ExprKind::BOX:
        case ExprKind::UNBOX:
        case ExprKind::GET_EXCEPTION:
        case ExprKind::RAW_ARRAY_LITERAL_INIT:
        case ExprKind::RAW_ARRAY_INIT_BY_VALUE:
        case ExprKind::VARRAY:
        case ExprKind::VARRAY_BUILDER:
        case ExprKind::TRANSFORM_TO_GENERIC:
        case ExprKind::TRANSFORM_TO_CONCRETE:
        case ExprKind::UNBOX_TO_REF:
        case ExprKind::GET_RTTI:
            exprKind[GetId<Expression>(&obj) - 1] = PackageFormat::ExpressionElem_Expression;
            return Serialize<PackageFormat::Expression>(static_cast<const Expression&>(obj)).Union();
        case ExprKind::INVALID:
        case ExprKind::MAX_EXPR_KINDS:
            CJC_ABORT();
            return 0;
        default:
            CJC_ABORT();
            return 0;
    }
}

template <> flatbuffers::Offset<void> CHIRSerializer::CHIRSerializerImpl::Dispatch(const CustomTypeDef& obj)
{
    switch (obj.GetCustomKind()) {
        case CustomDefKind::TYPE_STRUCT:
            defKind[GetId<CustomTypeDef>(&obj) - 1] = PackageFormat::CustomTypeDefElem_StructDef;
            return Serialize<PackageFormat::StructDef>(static_cast<const StructDef&>(obj)).Union();
        case CustomDefKind::TYPE_ENUM:
            defKind[GetId<CustomTypeDef>(&obj) - 1] = PackageFormat::CustomTypeDefElem_EnumDef;
            return Serialize<PackageFormat::EnumDef>(static_cast<const EnumDef&>(obj)).Union();
        case CustomDefKind::TYPE_CLASS:
            defKind[GetId<CustomTypeDef>(&obj) - 1] = PackageFormat::CustomTypeDefElem_ClassDef;
            return Serialize<PackageFormat::ClassDef>(static_cast<const ClassDef&>(obj)).Union();
        case CustomDefKind::TYPE_EXTEND:
            defKind[GetId<CustomTypeDef>(&obj) - 1] = PackageFormat::CustomTypeDefElem_ExtendDef;
            return Serialize<PackageFormat::ExtendDef>(static_cast<const ExtendDef&>(obj)).Union();
        default:
            CJC_ABORT();
            return 0;
    }
}

void CHIRSerializer::CHIRSerializerImpl::Dispatch()
{
    while (!(typeQueue.empty() && valueQueue.empty() && exprQueue.empty() && defQueue.empty())) {
        while (!typeQueue.empty()) {
            auto type = typeQueue.front();
            allType[GetId<Type>(type) - 1] = Dispatch<Type>(*type);
            typeQueue.pop();
        }
        while (!valueQueue.empty()) {
            auto value = valueQueue.front();
            allValue[GetId<Value>(value) - 1] = Dispatch<Value>(*value);
            valueQueue.pop_front();
        }
        while (!exprQueue.empty()) {
            auto expr = exprQueue.front();
            allExpression[GetId<Expression>(expr) - 1] = Dispatch<Expression>(*expr);
            exprQueue.pop();
        }
        while (!defQueue.empty()) {
            auto def = defQueue.front();
            allCustomTypeDef[GetId<CustomTypeDef>(def) - 1] = Dispatch<CustomTypeDef>(*def);
            defQueue.pop_front();
        }
    }
    packageInitFunc = GetId<Value>(package.GetPackageInitFunc());
    packageLiteralInitFunc = GetId<Value>(package.GetPackageLiteralInitFunc());
}

// ========================== Utilities ==========================================

void CHIRSerializer::CHIRSerializerImpl::Save(const std::string& filename, ToCHIR::Phase phase)
{
    auto accesslevel = package.GetPackageAccessLevel();
    auto packageName = package.GetName();
    auto serializedPackage = PackageFormat::CreateCHIRPackageDirect(builder, packageName.c_str(), "",
        PackageFormat::PackageAccessLevel(accesslevel), &typeKind, &allType, &valueKind, &allValue, &exprKind,
        &allExpression, &defKind, &allCustomTypeDef, packageInitFunc, PackageFormat::Phase(phase),
        packageLiteralInitFunc);

    builder.Finish(serializedPackage);
    const uint8_t* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    std::ofstream output(filename, std::ios::out | std::ofstream::binary);
    CJC_ASSERT(output.is_open());
    output.write(reinterpret_cast<const char*>(buf), static_cast<long>(size));
    output.close();
}

void CHIRSerializer::CHIRSerializerImpl::Initialize()
{
    for (auto value : package.GetGlobalVars()) {
        valueQueue.push_back(value);
    }
    for (auto value : package.GetGlobalFunctions()) {
        valueQueue.push_back(value);
    }
    for (auto def : package.GetAllCustomTypeDef()) {
        defQueue.push_back(def);
    }

    // allocate def id earlier
    for (auto def : std::as_const(defQueue)) {
        def2Id[def] = ++defCount;
        allCustomTypeDef.emplace_back(0);
        defKind.emplace_back(0);
    }

    // allocate value id earlier
    for (auto obj : std::as_const(valueQueue)) {
        value2Id[obj] = ++valueCount;
        allValue.emplace_back(0);
        valueKind.emplace_back(0);
    }
}

flatbuffers::DetachedBuffer CHIRSerializer::CHIRSerializerImpl::ConvertToMemoryData()
{
    auto accesslevel = package.GetPackageAccessLevel();
    auto packageName = package.GetName();
    auto serializedPackage = PackageFormat::CreateCHIRPackageDirect(builder, packageName.c_str(), "",
        PackageFormat::PackageAccessLevel(accesslevel), &typeKind, &allType, &valueKind, &allValue, &exprKind,
        &allExpression, &defKind, &allCustomTypeDef, packageInitFunc, PackageFormat::Phase::Phase_RAW,
        packageLiteralInitFunc);

    builder.Finish(serializedPackage);
    return builder.Release();
}