// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Utils.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/AttributePack.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Mangle/BaseMangler.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "cangjie/Sema/TypeManager.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/Utils/ConstantsUtils.h"

namespace Cangjie::Native::FFI {

using namespace TypeCheckUtil;

OwnedPtr<RefExpr> CreateThisRef(Ptr<Decl> target, Ptr<Ty> ty, Ptr<File> curFile)
{
    auto thisRef = MakeOwned<RefExpr>();
    thisRef->isThis = true;
    thisRef->SetTy(ty);
    thisRef->ref.identifier = SrcIdentifier("this");
    thisRef->ref.target = target;
    thisRef->curFile = curFile;
    return thisRef;
}

OwnedPtr<CallExpr> CreateThisCall(
    Decl& target, FuncDecl& baseTarget, Ptr<Ty> funcTy, Ptr<File> curFile, std::vector<OwnedPtr<FuncArg>> args)
{
    auto call = CreateCallExpr(CreateThisRef(Ptr(&baseTarget), funcTy, curFile), std::move(args));
    call->callKind = CallKind::CALL_OBJECT_CREATION;
    call->SetTy(target.GetTy());
    call->resolvedFunction = Ptr(&baseTarget);

    return call;
}

OwnedPtr<PrimitiveType> CreateUnitType(Ptr<File> curFile)
{
    auto ret = MakeOwned<PrimitiveType>();
    ret->str = "Unit";
    ret->kind = TypeKind::TYPE_UNIT;
    ret->SetTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    ret->curFile = curFile;

    return ret;
}

std::vector<Ptr<Ty>> GetParamTys(FuncParamList& params)
{
    std::vector<Ptr<Ty>> paramTys;

    for (auto& param : params.params) {
        paramTys.push_back(param->GetTy());
    }
    return paramTys;
}

OwnedPtr<RefExpr> CreateSuperRef(Ptr<Decl> target, Ptr<Ty> ty)
{
    auto superRef = MakeOwned<RefExpr>();
    superRef->isSuper = true;
    superRef->SetTy(ty);
    superRef->ref.identifier = SrcIdentifier("super");
    superRef->ref.target = target;
    return superRef;
}

OwnedPtr<CallExpr> CreateSuperCall(Decl& target, FuncDecl& baseTarget, Ptr<Ty> funcTy)
{
    auto call = CreateCallExpr(CreateSuperRef(Ptr(&baseTarget), funcTy), {});
    call->callKind = CallKind::CALL_SUPER_FUNCTION;
    call->SetTy(target.GetTy());
    call->resolvedFunction = Ptr(&baseTarget);

    return call;
}

OwnedPtr<Type> CreateType(Ptr<Ty> ty)
{
    auto res = MakeOwned<Type>();
    res->SetTy(ty);
    return res;
}

OwnedPtr<Type> CreateFuncType(Ptr<FuncTy> ty)
{
    auto res = MakeOwned<FuncType>();
    res->SetTy(ty);

    for (auto param : ty->paramTys) {
        res->paramTypes.push_back(CreateType(param));
    }

    return res;
}

OwnedPtr<Expr> CreateBoolMatch(
    OwnedPtr<Expr> selector, OwnedPtr<Expr> trueBranch, OwnedPtr<Expr> falseBranch, Ptr<Ty> ty)
{
    static const auto BOOL_TY = TypeManager::GetPrimitiveTy(TypeKind::TYPE_BOOLEAN);

    OwnedPtr<ConstPattern> truePattern = MakeOwned<ConstPattern>();
    truePattern->literal = CreateLitConstExpr(LitConstKind::BOOL, "true", BOOL_TY);
    truePattern->SetTy(BOOL_TY);

    OwnedPtr<ConstPattern> falsePattern = MakeOwned<ConstPattern>();
    falsePattern->literal = CreateLitConstExpr(LitConstKind::BOOL, "false", BOOL_TY);
    falsePattern->SetTy(BOOL_TY);

    auto caseTrue = CreateMatchCase(std::move(truePattern), std::move(trueBranch));
    auto caseFalse = CreateMatchCase(std::move(falsePattern), std::move(falseBranch));

    std::vector<OwnedPtr<MatchCase>> matchCases;
    matchCases.emplace_back(std::move(caseTrue));
    matchCases.emplace_back(std::move(caseFalse));
    auto curFile = selector->curFile;
    return WithinFile(CreateMatchExpr(std::move(selector), std::move(matchCases), ty), curFile);
}

StructDecl& GetStringDecl(const ImportManager& importManager)
{
    static auto decl = importManager.GetCoreDecl<StructDecl>(STD_LIB_STRING);
    CJC_NULLPTR_CHECK(decl);
    return *decl;
}

OwnedPtr<CallExpr> WrapReturningLambdaCall(TypeManager& typeManager, std::vector<OwnedPtr<Node>> nodes)
{
    CJC_ASSERT_WITH_MSG(!nodes.empty(), "cannot create lambda with empty body");
    auto retTy = nodes.back()->GetTy();
    auto lambda = WrapReturningLambdaExpr(typeManager, std::move(nodes));
    return CreateCallExpr(std::move(lambda), {}, nullptr, retTy);
}

OwnedPtr<LambdaExpr> WrapUnitLambdaExpr(
    TypeManager& typeManager, std::vector<OwnedPtr<Node>> nodes, std::vector<OwnedPtr<FuncParam>> lambdaParams)
{
    auto unitLiteral = CreateUnitExpr(TypeManager::GetPrimitiveTy(AST::TypeKind::TYPE_UNIT));
    nodes.push_back(std::move(unitLiteral));

    return WrapReturningLambdaExpr(typeManager, std::move(nodes), std::move(lambdaParams));
}

OwnedPtr<LambdaExpr> WrapReturningLambdaExpr(
    TypeManager& typeManager, std::vector<OwnedPtr<Node>> nodes, std::vector<OwnedPtr<FuncParam>> lambdaParams)
{
    CJC_ASSERT(!nodes.empty());
    auto curFile = nodes[0]->curFile;
    std::vector<Ptr<Ty>> lambdaParamTys;
    std::transform(lambdaParams.begin(), lambdaParams.end(), std::back_inserter(lambdaParamTys),
        [](auto& p) { return p->GetTy(); });
    auto paramLists = Nodes<FuncParamList>(CreateFuncParamList(std::move(lambdaParams)));
    auto retTy = nodes.back()->GetTy();
    auto unsafeBlock = CreateBlock(Nodes(ASTCloner::Clone(Ptr(As<ASTKind::EXPR>(nodes.back().get())))), retTy);
    unsafeBlock->EnableAttr(Attribute::UNSAFE);
    auto retExpr = CreateReturnExpr(std::move(unsafeBlock));
    retExpr->SetTy(TypeManager::GetNothingTy());
    nodes.pop_back();
    auto lambda =
        CreateLambdaExpr(CreateFuncBody(std::move(paramLists), nullptr, CreateBlock(std::move(nodes), retTy), retTy));
    retExpr->refFuncBody = lambda->funcBody.get();
    lambda->funcBody->body->body.push_back(std::move(retExpr));
    lambda->curFile = curFile;
    lambda->SetTy(typeManager.GetFunctionTy(std::move(lambdaParamTys), retTy));
    return lambda;
}

std::string GetCangjieLibName(const std::string& outputLibPath, const std::string& fullPackageName, bool trimmed)
{
    if (FileUtil::IsDir(outputLibPath)) {
        return fullPackageName;
    }
    auto outputFileName = FileUtil::GetFileName(outputLibPath);

    constexpr std::string_view libPrefix = "lib";
    // check if [outputLibPath] starts with [LIB_PREFIX]
    if (outputFileName.rfind(libPrefix, 0) == 0) {
        if (!trimmed) {
            return outputFileName;
        }

        size_t extIdx = outputFileName.find_last_of(".");
        if (extIdx == std::string::npos) {
            return fullPackageName;
        }
        return outputFileName.substr(libPrefix.size(), extIdx - libPrefix.size());
    }
    return fullPackageName;
}

std::string GetMangledMethodName(const BaseMangler& mangler, const std::vector<OwnedPtr<FuncParam>>& params,
    const std::string& methodName, TypeManager& typeManager, GenericConfigInfo* genericConfig)
{
    std::string name(methodName);

    for (auto& param : params) {
        auto paramTy = param->GetTy();
        if (genericConfig && param->GetTy()->HasGeneric()) {
            paramTy = GetGenericInstTy(genericConfig, param->GetTy(), typeManager);
        }
        std::string mangledParam = mangler.MangleType(*paramTy);
        std::replace(mangledParam.begin(), mangledParam.end(), '.', '_');
        name += mangledParam;
    }

    return name;
}

Ptr<Annotation> GetForeignNameAnnotation(const Decl& decl)
{
    return FindFirstAnnotation(decl, AnnotationKind::FOREIGN_NAME);
}

bool IsSuperConstructorCall(const CallExpr& call)
{
    auto baseFunc = As<ASTKind::REF_EXPR>(call.baseFunc.get());
    if (!baseFunc || !baseFunc->isSuper) {
        return false;
    }
    return call.callKind == CallKind::CALL_SUPER_FUNCTION;
}

Ptr<Annotation> GetAnnotation(const Decl& decl, AnnotationKind annotationKind)
{
    auto it = std::find_if(decl.annotations.begin(), decl.annotations.end(),
        [annotationKind](const auto& anno) { return anno->kind == annotationKind; });
    return it != decl.annotations.end() ? it->get() : nullptr;
}

Ptr<std::string> GetSingleArgumentAnnotationValue(const Decl& target, AnnotationKind annotationKind)
{
    for (auto& anno : target.annotations) {
        if (anno->kind != annotationKind) {
            continue;
        }

        if (anno->args.size() != 1) {
            break;
        }

        CJC_ASSERT(anno->args.size() == 1);
        CJC_ASSERT(anno->args[0]->expr->astKind == ASTKind::LIT_CONST_EXPR);
        auto lce = As<ASTKind::LIT_CONST_EXPR>(anno->args[0]->expr.get());
        CJC_ASSERT(lce);

        return &lce->stringValue;
    }

    return nullptr;
}

std::string GetObjCMirrorForeignName(const ClassLikeDecl& target)
{
    if (auto customName = GetSingleArgumentAnnotationValue(target, AnnotationKind::OBJ_C_MIRROR)) {
        return *customName;
    }
    return target.identifier.Val();
}

bool IsObjCGeneratedNSStringCtor(const Decl& target)
{
    auto funcDecl = DynamicCast<const FuncDecl>(&target);
    if (!funcDecl || funcDecl->identifier.Val() != INIT_IDENT ||
        !funcDecl->TestAttr(Attribute::CONSTRUCTOR, Attribute::COMPILER_ADD)) {
        return false;
    }

    CJC_ASSERT(funcDecl->funcBody);
    auto& paramLists = funcDecl->funcBody->paramLists;
    CJC_ASSERT(paramLists.size() > 0);
    auto& params = paramLists[0]->params;
    if (params.size() != 1 || params[0]->type->symbol->name != STD_LIB_STRING) {
        return false;
    }

    return true;
}

bool IsObjCGeneratedNSObjectToString(const Decl& target)
{
    auto funcDecl = DynamicCast<const FuncDecl>(&target);
    if (!funcDecl || funcDecl->identifier.Val() != TOSTRING_METHOD_IDENT ||
        !funcDecl->TestAttr(Attribute::COMPILER_ADD) || !funcDecl->funcBody) {
        return false;
    }

    CJC_ASSERT(funcDecl->funcBody);
    auto& paramLists = funcDecl->funcBody->paramLists;
    CJC_ASSERT(paramLists.size() > 0);
    auto& params = paramLists[0]->params;
    if (params.size() != 0) {
        return false;
    }

    return true;
}

bool IsObjCGeneratedMember(const Decl& target)
{
    auto classLikeDecl = DynamicCast<const ClassLikeDecl>(target.outerDecl);
    if (!classLikeDecl) {
        return false;
    }
    auto foreignName = GetObjCMirrorForeignName(*classLikeDecl);
    return (foreignName == NSSTRING_CLASS_IDENT && IsObjCGeneratedNSStringCtor(target)) ||
        (foreignName == NSOBJECT_CLASS_IDENT && IsObjCGeneratedNSObjectToString(target));
}

OwnedPtr<PrimitiveType> GetPrimitiveType(std::string typeName, AST::TypeKind typekind)
{
    OwnedPtr<PrimitiveType> type = MakeOwned<PrimitiveType>();
    type->str = typeName;
    type->kind = typekind;
    type->SetTy(TypeManager::GetPrimitiveTy(typekind));
    return type;
}

bool IsCJMappingGeneric(const Decl& decl)
{
    auto classDecl = DynamicCast<ClassDecl*>(&decl);
    if (classDecl && !classDecl->TestAnyAttr(AST::Attribute::ABSTRACT, AST::Attribute::OPEN) &&
        classDecl->GetTy()->HasGeneric()) {
        return true;
    }

    auto structDecl = DynamicCast<StructDecl*>(&decl);
    if (structDecl && structDecl->GetTy()->HasGeneric()) {
        return true;
    }

    auto enumDecl = DynamicCast<EnumDecl*>(&decl);
    if (enumDecl && enumDecl->GetTy()->HasGeneric()) {
        return true;
    }

    auto interfaceDecl = DynamicCast<InterfaceDecl*>(&decl);
    if (interfaceDecl && interfaceDecl->GetTy()->HasGeneric()) {
        return true;
    }
    return false;
}

void SplitAndTrim(std::string str, std::vector<std::string>& types)
{
    size_t pos = str.find(',');
    if (pos == std::string::npos) {
        types.push_back(str);
        return;
    }
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        types.push_back(token);
    }
}

std::string JoinVector(const std::vector<std::string>& vec, const std::string& delimiter)
{
    std::string result;
    for (size_t i = 0; i < vec.size(); ++i) {
        result += vec[i];
        if (i != vec.size() - 1) {
            result += delimiter;
        }
    }
    return result;
}

void InitGenericConfigs(
    const File& file, const AST::Decl* decl, std::vector<GenericConfigInfo*>& genericConfigs, bool& isGenericGlueCode)
{
    // Collect information on the names of generic configuration methods
    // such as: {GenericClass<int32>, symbols: ["find", "value"]>}
    std::unordered_map<std::string, std::unordered_set<std::string>> visibleFuncs;
    for (const auto& outerPair : file.curPackage->allowedInteropCJGenericInstantiations) {
        const auto declSymbolName = outerPair.first;
        const auto& innerMap = outerPair.second;
        if (declSymbolName != decl->identifier.Val()) {
            continue;
        }
        for (const auto& innerPair : innerMap) {
            const std::string typeStr = innerPair.first;
            const GenericTypeArguments& args = innerPair.second;
            std::unordered_set<std::string> funcNames = args.symbols;
            std::vector<std::string> actualTypes;
            SplitAndTrim(typeStr, actualTypes);
            std::vector<std::pair<std::string, std::string>> instTypes;
            const auto typeArgs = decl->GetTy()->typeArgs;
            for (size_t i = 0; i < typeArgs.size(); i++) {
                instTypes.push_back(std::make_pair(typeArgs[i]->name, actualTypes[i]));
            }
            std::string declName = decl->identifier.Val();
            std::string declWInstStr = declName + JoinVector(actualTypes);
            GenericConfigInfo* declGenericConfig = new GenericConfigInfo(declName, declWInstStr, instTypes, funcNames);
            genericConfigs.push_back(declGenericConfig);
            if (!isGenericGlueCode) {
                isGenericGlueCode = true;
            }
        }
    }
}

std::string GetGenericActualType(const GenericConfigInfo* config, std::string genericName)
{
    CJC_ASSERT(config);
    for (size_t i = 0; i < config->instTypes.size(); ++i) {
        if (config->instTypes[i].first == genericName) {
            std::string instType = config->instTypes[i].second;
            return instType;
        }
    }
    return "";
}

// Current generic just support primitive type
TypeKind GetActualTypeKind(std::string configType)
{
    static const std::unordered_map<std::string, TypeKind> typeMap = {{"Int", TypeKind::TYPE_INT64},
        {"Int8", TypeKind::TYPE_INT8}, {"Int16", TypeKind::TYPE_INT16}, {"Int32", TypeKind::TYPE_INT32},
        {"Int64", TypeKind::TYPE_INT64}, {"IntNative", TypeKind::TYPE_INT_NATIVE}, {"UInt8", TypeKind::TYPE_UINT8},
        {"UInt16", TypeKind::TYPE_UINT16}, {"UInt32", TypeKind::TYPE_UINT32}, {"UInt64", TypeKind::TYPE_UINT64},
        {"UIntNative", TypeKind::TYPE_UINT_NATIVE}, {"Float16", TypeKind::TYPE_FLOAT16},
        {"Float32", TypeKind::TYPE_FLOAT32}, {"Float64", TypeKind::TYPE_FLOAT64}, {"Bool", TypeKind::TYPE_BOOLEAN},
        {"Boolean", TypeKind::TYPE_BOOLEAN}, {"Unit", TypeKind::TYPE_UNIT}};
    auto it = typeMap.find(configType);
    CJC_ASSERT(it != typeMap.end());
    return it->second;
}

Ptr<Ty> GetGenericInstTy(const GenericConfigInfo* config, std::string genericName)
{
    auto actualTypeName = GetGenericActualType(config, genericName);
    if (actualTypeName.empty()) {
        return Ty::GetInitialTy();
    }
    return GetTyByName(actualTypeName);
}

Ptr<Ty> GetGenericInstTy(const GenericConfigInfo* config, const Ptr<Ty>& genericTy, TypeManager& typeManager)
{
    switch (genericTy->kind) {
        case TypeKind::TYPE_GENERICS:
            return GetGenericInstTy(config, genericTy->name);
        case TypeKind::TYPE_FUNC: {
            auto funcTy = StaticCast<FuncTy*>(genericTy);
            CJC_NULLPTR_CHECK(funcTy);

            std::vector<Ptr<Ty>> paramTys;
            Ptr<Ty> retTy = funcTy->retTy;
            if (funcTy->retTy && funcTy->retTy->HasGeneric()) {
                retTy = GetGenericInstTy(config, funcTy->retTy, typeManager);
            }

            for (auto& paramTy : funcTy->paramTys) {
                if (paramTy && paramTy->HasGeneric()) {
                    paramTys.push_back(GetGenericInstTy(config, paramTy, typeManager));
                } else {
                    paramTys.push_back(paramTy);
                }
            }
            auto actualFuncTy = typeManager.GetFunctionTy(paramTys, retTy);
            return actualFuncTy;
        }
        case TypeKind::TYPE_TUPLE: {
            std::vector<Ptr<Ty>> elements;
            for (const auto& it : genericTy->typeArgs) {
                if (it->IsGeneric()) {
                    elements.emplace_back(GetGenericInstTy(config, it->name));
                    continue;
                }
                elements.emplace_back(it);
            }
            auto actualTy = typeManager.GetTupleTy(elements);
            return actualTy;
        }
        default:
            return genericTy;
    }
}

Ptr<Ty> GetTyByName(std::string typeStr)
{
    auto typeKind = GetActualTypeKind(typeStr);
    // Current only support primitive type.
    auto ty = TypeManager::GetPrimitiveTy(typeKind);
    return ty;
}

OwnedPtr<Type> GetGenericInstType(const GenericConfigInfo* config, std::string genericName)
{
    auto actualTypeName = GetGenericActualType(config, genericName);
    return GetTypeByName(actualTypeName);
}

OwnedPtr<Type> GetTypeByName(std::string typeStr)
{
    auto typeKind = GetActualTypeKind(typeStr);
    // Current only support primitive type.
    auto type = GetPrimitiveType(typeStr, typeKind);
    return type;
}

OwnedPtr<Type> GetGenericInstType(const GenericConfigInfo* config, const Ptr<Ty>& genericTy, TypeManager& typeManager)
{
    auto ty = GetGenericInstTy(config, genericTy, typeManager);
    if (ty->IsPrimitive()) {
        return GetPrimitiveType(ty->String(), ty->kind);
    }

    if (ty->IsTuple()) {
        OwnedPtr<TupleType> type = MakeOwned<TupleType>();
        type->SetTy(ty);
        return type;
    }

    if (ty->IsFunc()) {
        auto funcTy = StaticCast<FuncTy*>(ty);
        CJC_NULLPTR_CHECK(funcTy);
        auto res = MakeOwned<FuncType>();
        res->SetTy(ty);
        for (auto param : funcTy->paramTys) {
            res->paramTypes.push_back(GetGenericInstType(config, param, typeManager));
        }
        res->retType = GetGenericInstType(config, funcTy->retTy, typeManager);
        return res;
    }
    CJC_ASSERT(false);
    auto type = MakeOwned<InvalidType>(Position());
    return type;
}

bool IsThisConstructorCall(const CallExpr& call)
{
    auto baseFunc = As<ASTKind::REF_EXPR>(call.baseFunc.get());
    if (!baseFunc || !baseFunc->isThis) {
        return false;
    }
    // this(...) call is a kind of CALL_DECLARED_FUNCTION
    return call.callKind == CallKind::CALL_DECLARED_FUNCTION;
}

void ReplaceGenericTyForFunc(Ptr<FuncDecl> funcDecl, GenericConfigInfo* genericConfig, TypeManager& typeManager)
{
    std::vector<Ptr<Ty>> tmpParamTys;
    std::vector<Ptr<Ty>> tmpTypeArgs;
    auto& retType = *funcDecl->funcBody->retType;
    if (retType.GetTy()->HasGeneric()) {
        funcDecl->funcBody->retType = GetGenericInstType(genericConfig, retType.GetTy(), typeManager);
    }

    for (auto& param : funcDecl->funcBody->paramLists[0]->params) {
        if (param->type->GetTy() && param->type->GetTy()->HasGeneric()) {
            param->type = GetGenericInstType(genericConfig, param->GetTy(), typeManager);
            param->SetTy(GetGenericInstTy(genericConfig, param->GetTy(), typeManager));
        }
        tmpParamTys.push_back(param->GetTy());
    }
    for (auto& typeArg : funcDecl->GetTy()->typeArgs) {
        if (typeArg->HasGeneric()) {
            tmpTypeArgs.push_back(GetGenericInstTy(genericConfig, typeArg, typeManager));
        } else {
            tmpTypeArgs.push_back(typeArg);
        }
    }
    auto funcTy = typeManager.GetFunctionTy(tmpParamTys, funcDecl->funcBody->retType->GetTy());
    funcTy->typeArgs = tmpTypeArgs;
    funcDecl->SetTy(funcTy);
}

// Match generic parameters in all function parameters to their corresponding Ptr<Ty>.
void GetArgsAndRetGenericActualTyVector(const GenericConfigInfo* config, FuncDecl& ctor,
    std::unordered_map<std::string, Ptr<Ty>>& actualTyArgMap, std::vector<Ptr<Ty>>& funcTyParams,
    std::vector<OwnedPtr<Type>>& actualPrimitiveType, TypeManager& typeManager)
{
    if (ctor.outerDecl) {
        for (auto argTy : ctor.outerDecl->GetTy()->typeArgs) {
            if (argTy->IsGeneric()) {
                auto actualRetTy = GetGenericInstTy(config, argTy, typeManager);
                actualTyArgMap[argTy->name] = actualRetTy;
                actualPrimitiveType.emplace_back(GetTypeByName(actualRetTy->String()));
            }
        }
    }

    // Analyze generic parameters within inner functions.
    for (size_t argIdx = 0; argIdx < ctor.funcBody->paramLists[0]->params.size(); ++argIdx) {
        auto& arg = ctor.funcBody->paramLists[0]->params[argIdx];
        if (arg->GetTy()->HasGeneric()) {
            if (auto actualTy = GetGenericInstTy(config, arg->GetTy(), typeManager)) {
                funcTyParams.emplace_back(actualTy);
            } else {
                funcTyParams.emplace_back(arg->GetTy());
            }
        } else {
            funcTyParams.emplace_back(arg->GetTy());
        }
    }
}

Ptr<Ty> GetInstantyForGenericTy(
    Decl& decl, const std::unordered_map<std::string, Ptr<Ty>>& actualTyArgMap, TypeManager& typeManager)
{
    std::vector<Ptr<Ty>> actualTypeArgs;
    for (const auto& typeArg : decl.GetTy()->typeArgs) {
        std::string typeArgName = typeArg->name;

        auto it = actualTyArgMap.find(typeArgName);
        if (it != actualTyArgMap.end()) {
            actualTypeArgs.emplace_back(it->second);
        }
    }

    Ptr<Ty> instantTy;
    auto classDecl = As<ASTKind::CLASS_DECL>(&decl);
    if (classDecl) {
        instantTy = typeManager.GetClassTy(*classDecl, actualTypeArgs);
    }
    auto structDecl = As<ASTKind::STRUCT_DECL>(&decl);
    if (structDecl) {
        instantTy = typeManager.GetStructTy(*structDecl, actualTypeArgs);
    }
    auto enumDecl = As<ASTKind::ENUM_DECL>(&decl);
    if (enumDecl) {
        instantTy = typeManager.GetEnumTy(*enumDecl, actualTypeArgs);
    }
    auto interfaceDecl = As<ASTKind::INTERFACE_DECL>(&decl);
    if (interfaceDecl) {
        instantTy = typeManager.GetInterfaceTy(*interfaceDecl, actualTypeArgs);
    }
    return instantTy;
}

bool IsGenericParam(const Ptr<Ty> ty, const AST::Decl& decl, Native::FFI::GenericConfigInfo* genericConfig)
{
    if (!IsCJMappingGeneric(decl) || !ty->HasGeneric()) {
        return false;
    }
    if (ty->IsGeneric()) {
        return !GetGenericActualType(genericConfig, ty->name).empty();
    }
    if (ty->IsTuple()) {
        bool result = true;
        for (auto it : ty->typeArgs) {
            if (it->IsGeneric()) {
                result &= IsGenericParam(it, decl, genericConfig);
            }
        }
        return result;
    }
    if (ty->IsFunc()) {
        bool result = true;
        auto funTy = StaticCast<FuncTy*>(ty);
        CJC_NULLPTR_CHECK(funTy);
        for (auto paramTy : funTy->paramTys) {
            if (paramTy->IsGeneric()) {
                result &= IsGenericParam(paramTy, decl, genericConfig);
            }
        }
        CJC_NULLPTR_CHECK(funTy->retTy);
        if (funTy->retTy->IsGeneric()) {
            result &= IsGenericParam(funTy->retTy, decl, genericConfig);
        }
        return result;
    }
    return false;
}

bool IsVisibalFunc(const FuncDecl& funcDecl, const AST::Decl& decl, Native::FFI::GenericConfigInfo* genericConfig)
{
    bool hasGenericParm = false;
    auto& params = funcDecl.funcBody->paramLists[0]->params;
    auto retTy = StaticCast<FuncTy&>(*funcDecl.GetTy()).retTy;
    for (auto& param : params) {
        if (IsGenericParam(param->GetTy(), decl, genericConfig)) {
            hasGenericParm = true;
            break;
        }
    }
    if (!hasGenericParm) {
        hasGenericParm = IsGenericParam(retTy, decl, genericConfig);
    }

    if (!hasGenericParm) {
        return true;
    }

    bool isVisibalFunc = genericConfig->funcNames.count(funcDecl.identifier.Val()) > 0;
    return hasGenericParm && isVisibalFunc;
}

std::string GetCjMappingTupleName(const Ty& tupleTy)
{
    CJC_ASSERT(tupleTy.IsTuple());
    std::string name("TupleOf");
    for (auto it : tupleTy.typeArgs) {
        name += it->String();
    }
    return name;
}

std::string GetLambdaJavaClassName(LambdaPattern& pattern)
{
    std::string name = "";
    for (auto& type : pattern.parameterTypes) {
        name += type;
    }
    name = name + "To" + pattern.returnType;
    return name;
}

std::string GetLambdaJavaClassName(Ptr<Ty> ty)
{
    auto funTy = StaticCast<FuncTy*>(ty);
    CJC_NULLPTR_CHECK(funTy);
    std::string name = "";
    for (auto paramTy : funTy->paramTys) {
        CJC_NULLPTR_CHECK(paramTy);
        std::string paramStr = paramTy->String();
        name += paramStr;
    }
    CJC_NULLPTR_CHECK(funTy->retTy);
    std::string retStr = funTy->retTy->String();
    name = name + "To" + retStr;
    return name;
}

ClassDecl& GetExceptionDecl(const ImportManager& importManager)
{
    const static auto exception = [&] {
        const auto exceptionDecl = importManager.GetCoreDecl("Exception");
        CJC_NULLPTR_CHECK(exceptionDecl);

        ClassDecl* res = nullptr;
        if (auto ex = As<ASTKind::CLASS_DECL>(exceptionDecl)) {
            res = ex;
        } else {
            CJC_ABORT_WITH_MSG("'Exception' declaration expected to be 'ClassDecl'");
        }

        return res;
    }();

    return *exception;
}

OwnedPtr<ThrowExpr> CreateThrowExceptionCall(const ImportManager& importManager,
    TypeManager& typeManager, const std::string& msg, Ptr<File> curFile)
{
    auto exceptionArgs = [&] {
        auto exceptionMsg =
            WithinFile(CreateLitConstExpr(LitConstKind::STRING, msg, GetStringDecl(importManager).GetTy()), curFile);
        std::vector<OwnedPtr<Expr>> res;
        res.emplace_back(std::move(exceptionMsg));
        return res;
    }();
    const auto& exception = GetExceptionDecl(importManager);

    return CreateThrowException(exception, std::move(exceptionArgs), *curFile, typeManager);
}

bool AreParamTypeKindsValid(const FuncDecl& fd, const std::vector<TypeKind>& typeKinds)
{
    if (!fd.funcBody || fd.funcBody->paramLists[0]->params.size() != typeKinds.size()) {
        return false;
    }
    for (size_t i = 0; i < typeKinds.size(); ++i) {
        CJC_NULLPTR_CHECK(fd.funcBody->paramLists[0]->params[i]->GetTy());
        if (fd.funcBody->paramLists[0]->params[i]->GetTy()->kind != typeKinds[i]) {
            return false;
        }
    }
    return true;
}

} // namespace Cangjie::Native::FFI
