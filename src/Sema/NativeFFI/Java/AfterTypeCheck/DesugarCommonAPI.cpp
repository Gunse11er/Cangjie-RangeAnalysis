// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "Utils.h"

#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/Utils/SafePointer.h"

namespace Cangjie::Interop::Java {
using namespace Cangjie::Native::FFI;

OwnedPtr<FuncParam> JavaDesugarManager::CreateJniEnvParam(const std::string& name)
{
    static auto jniEnvPtrDecl = lib.GetJniEnvPtrDecl();
    static auto jniEnvPtrTy = lib.GetJNIEnvPtrTy();
    CJC_NULLPTR_CHECK(jniEnvPtrDecl);
    return CreateFuncParam(name, ASTCloner::Clone(jniEnvPtrDecl->type.get()), nullptr, jniEnvPtrTy);
}

OwnedPtr<FuncParam> JavaDesugarManager::CreateJniJobjectOrJclassParam(const std::string& name)
{
    static auto jobjectTy = lib.GetJobjectTy();
    return CreateFuncParam(name, lib.CreateJobjectType(), nullptr, jobjectTy);
}

OwnedPtr<FuncParam> JavaDesugarManager::CreateRegistryIdParam(const std::string& name)
{
    static auto registryIdParamTy = lib.GetJlongTy();
    return CreateFuncParam(name, lib.CreateJlongType(), nullptr, registryIdParamTy);
}

FuncParam& JavaDesugarManager::PushEnvParams(std::vector<OwnedPtr<FuncParam>>& params, const std::string& name)
{
    auto param = CreateJniEnvParam(name);
    auto& paramRef = *param;
    params.push_back(std::move(param));
    return paramRef;
}

FuncParam& JavaDesugarManager::PushObjParams(std::vector<OwnedPtr<FuncParam>>& params, const std::string& name)
{
    auto param = CreateFuncParam(name, lib.CreateJobjectType(), nullptr, lib.GetJobjectTy());
    auto& paramRef = *param;
    params.push_back(std::move(param));
    return paramRef;
}

FuncParam& JavaDesugarManager::PushSelfParams(std::vector<OwnedPtr<FuncParam>>& params, std::string name)
{
    auto param = CreateRegistryIdParam(name);
    auto& paramRef = *param;
    params.push_back(std::move(param));
    return paramRef;
}

OwnedPtr<CallExpr> JavaDesugarManager::GetFwdClassInstance(OwnedPtr<RefExpr> paramRef, Decl& fwdClassDecl)
{
    Ptr<FuncDecl> ctor(nullptr);
    for (auto& member : fwdClassDecl.GetMemberDecls()) {
        if (auto fd = As<ASTKind::FUNC_DECL>(member); fd && fd->TestAttr(Attribute::CONSTRUCTOR)) {
            ctor = fd;
            break;
        }
    }
    CJC_ASSERT(ctor);

    auto curFile = fwdClassDecl.curFile;

    auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
    std::vector<OwnedPtr<FuncArg>> ctorCallArgs;
    ctorCallArgs.push_back(CreateFuncArg(std::move(entity)));

    auto fwdTy = fwdClassDecl.GetTy();
    auto fdRef = WithinFile(CreateRefExpr(*ctor), curFile);
    return CreateCallExpr(std::move(fdRef), std::move(ctorCallArgs), ctor, fwdTy, CallKind::CALL_OBJECT_CREATION);
}

Ptr<FuncDecl> JavaDesugarManager::CheckCjLambdaDeclByTy(Ptr<Ty> ty)
{
    auto funTy = StaticCast<FuncTy*>(ty);
    CJC_ASSERT(funTy);
    auto it = lambdaConfUtilFuncs.find(funTy->String());
    CJC_ASSERT(it != lambdaConfUtilFuncs.end() && "Error, please config lambda pattern");
    return it->second;
}

bool JavaDesugarManager::FillMethodParamsByArg(std::vector<OwnedPtr<FuncParam>>& params,
    std::vector<OwnedPtr<FuncArg>>& callArgs, FuncDecl& funcDecl, OwnedPtr<FuncParam>& arg, FuncParam& jniEnvPtrParam,
    Ptr<Ty> actualTy)
{
    CJC_NULLPTR_CHECK(funcDecl.curFile);
    Ptr<Ty> actualArgTy = arg->GetTy();
    actualArgTy = (actualTy && arg->GetTy()->HasGeneric()) ? actualTy : arg->GetTy();
    auto jniArgTy = GetJNITy(actualArgTy);
    OwnedPtr<FuncParam> param = CreateFuncParam(arg->identifier.GetRawText(), nullptr, nullptr, jniArgTy);
    auto classLikeTy = DynamicCast<ClassLikeTy*>(actualArgTy);
    if (classLikeTy && !classLikeTy->commonDecl) {
        return false;
    }
    auto outerDecl = funcDecl.outerDecl;
    auto paramRef = WithinFile(CreateRefExpr(*param), funcDecl.curFile);
    OwnedPtr<FuncArg> methodArg;
    if (IsMirror(*actualArgTy)) {
        auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
        methodArg = CreateFuncArg(CreateMirrorConstructorCall(importManager, std::move(entity), actualArgTy));
    } else if (IsImpl(*actualArgTy)) {
        auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
        methodArg = CreateFuncArg(lib.UnwrapJavaEntity(std::move(entity), actualArgTy, *outerDecl));
    } else if (actualArgTy->IsCoreOptionType() && (IsMirror(*actualArgTy->typeArgs[0]) ||
                                                   IsImpl(*actualArgTy->typeArgs[0]) ||
                                                   actualArgTy->typeArgs[0]->IsString())) {
        // funcDecl(Java_CFFI_JavaEntity(arg)) // if arg is null (as jobject == 0) -> java entity will preserve it
        auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
        methodArg = CreateFuncArg(lib.UnwrapJavaEntity(std::move(entity), actualArgTy, *outerDecl));
    } else if (IsCJMapping(*actualArgTy)) {
        auto entity = lib.CreateGetFromRegistryCall(
            WithinFile(CreateRefExpr(jniEnvPtrParam), funcDecl.curFile), std::move(paramRef), actualArgTy);
        methodArg = CreateFuncArg(WithinFile(std::move(entity), funcDecl.curFile));
    } else if (IsCJMappingInterface(*arg->GetTy())) {
        Ptr<Ty> fwdTy = TypeManager::GetInvalidTy();
        for (auto it : classLikeTy->directSubtypes) {
            if (it->name == classLikeTy->name + JAVA_FWD_CLASS_SUFFIX) {
                fwdTy = it;
                break;
            }
        }
        CJC_ASSERT(Ty::IsTyCorrect(fwdTy));

        auto fwdClassDecl = Ty::GetDeclOfTy(fwdTy);

        auto fwdClassInstance = GetFwdClassInstance(std::move(paramRef), *fwdClassDecl);
        methodArg = CreateFuncArg(WithinFile(std::move(fwdClassInstance), funcDecl.curFile));
    } else if (actualArgTy->IsTuple()) {
        if (IsCJMappingTuple(actualArgTy, tupleConfigs)) {
            auto entity = lib.CreateGetFromRegistryCall(
                WithinFile(CreateRefExpr(jniEnvPtrParam), funcDecl.curFile), std::move(paramRef), actualArgTy);
            methodArg = CreateFuncArg(WithinFile(std::move(entity), funcDecl.curFile));
        } else {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_cjmapping_method_arg_not_supported, *arg);
            funcDecl.EnableAttr(Attribute::IS_BROKEN);
            funcDecl.outerDecl->EnableAttr(Attribute::HAS_BROKEN);
            funcDecl.outerDecl->EnableAttr(Attribute::IS_BROKEN);
            return false;
        }
    } else if (actualArgTy->IsFunc()) {
        auto getCjLambdaFd = CheckCjLambdaDeclByTy(actualArgTy);
        auto getCjLambdaCallExpr = CreateCall(getCjLambdaFd, funcDecl.curFile, std::move(paramRef));
        methodArg = CreateFuncArg(std::move(getCjLambdaCallExpr));
    } else if (actualArgTy->IsString()) {
        // Convert JNI jobject (jstring) to Cangjie String:
        // jobject -> JavaEntity -> Struct-String.
        methodArg = CreateFuncArg(lib.JObjectToString(CreateRefExpr(*param), funcDecl.curFile));
    } else {
        methodArg = CreateFuncArg(std::move(paramRef));
    }

    params.push_back(std::move(param));
    callArgs.push_back(std::move(methodArg));
    return true;
}

OwnedPtr<Decl> JavaDesugarManager::GenerateNativeMethod(
    FuncDecl& sampleMethod, Decl& decl, const GenericConfigInfo* genericConfig)
{
    auto curFile = sampleMethod.curFile;
    CJC_NULLPTR_CHECK(curFile);

    Ptr<Ty> retTy = StaticCast<FuncTy*>(sampleMethod.GetTy())->retTy;
    std::vector<OwnedPtr<FuncParam>> params;
    PushEnvParams(params);
    // jobject or jclass
    PushObjParams(params, "_");
    CJC_ASSERT_WITH_MSG(!params.empty(), "jniEnvPtrParam is absent");
    auto& jniEnvPtrParam = *params[0];
    if (!sampleMethod.TestAttr(Attribute::STATIC)) {
        if (sampleMethod.TestAttr(Attribute::CJ_MIRROR_JAVA_INTERFACE_DEFAULT)) {
            PushObjParams(params, JAVA_SELF_OBJECT);
        } else {
            PushSelfParams(params);
        }
    }
    auto& selfParam = *params.back();
    std::vector<OwnedPtr<FuncArg>> methodCallArgs;
    std::unordered_map<std::string, Ptr<Ty>> actualTyArgMap;
    std::vector<Ptr<Ty>> funcTyParams;
    std::vector<OwnedPtr<Type>> actualPrimitiveType;
    if (genericConfig && genericConfig->instTypes.size() != 0) {
        GetArgsAndRetGenericActualTyVector(
            genericConfig, sampleMethod, actualTyArgMap, funcTyParams, actualPrimitiveType, typeManager);
    }
    auto instantTy = GetInstantyForGenericTy(decl, actualTyArgMap, typeManager);
    auto retActualTy = retTy->HasGeneric() ? GetGenericInstTy(genericConfig, retTy, typeManager) : retTy;
    CJC_ASSERT_WITH_MSG(!sampleMethod.funcBody->paramLists.empty(), "paramLists cannot be empty");
    for (auto& arg : sampleMethod.funcBody->paramLists[0]->params) {
        auto genericInstTy = GetGenericInstTy(genericConfig, arg->GetTy(), typeManager);
        if (!FillMethodParamsByArg(params, methodCallArgs, sampleMethod, arg, jniEnvPtrParam, genericInstTy)) {
            return nullptr;
        }
    }
    OwnedPtr<MemberAccess> methodAccess;
    if (sampleMethod.TestAttr(Attribute::STATIC)) {
        auto staticRefExpr = CreateRefExpr(decl);
        if (decl.GetTy()->HasGeneric()) {
            staticRefExpr->typeArguments = std::move(actualPrimitiveType);
            staticRefExpr->SetTy(instantTy);
        }
        methodAccess = CreateMemberAccess(WithinFile(std::move(staticRefExpr), curFile), sampleMethod);
    } else if (sampleMethod.TestAttr(Attribute::CJ_MIRROR_JAVA_INTERFACE_DEFAULT)) {
        auto& objParam = *params[2];
        auto paramRef = WithinFile(CreateRefExpr(objParam), curFile);
        auto fwdClassInstance = GetFwdClassInstance(std::move(paramRef), decl);
        methodAccess = CreateMemberAccess(std::move(fwdClassInstance), sampleMethod);
    } else {
        OwnedPtr<CallExpr> reg;
        if (decl.GetTy()->HasGeneric()) {
            reg = lib.CreateGetFromRegistryCall(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile),
                WithinFile(CreateRefExpr(selfParam), curFile), instantTy);
            methodAccess = CreateMemberAccess(std::move(reg), sampleMethod);
            methodAccess->SetTy(typeManager.GetFunctionTy(funcTyParams, retActualTy));
        } else {
            reg = lib.CreateGetFromRegistryCall(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile),
                WithinFile(CreateRefExpr(selfParam), curFile), decl.GetTy());
            methodAccess = CreateMemberAccess(std::move(reg), sampleMethod);
        }
    }
    methodAccess->curFile = curFile;
    auto methodCall = CreateCallExpr(std::move(methodAccess), std::move(methodCallArgs), Ptr(&sampleMethod),
        retActualTy, CallKind::CALL_DECLARED_FUNCTION);
    auto methodCallRes = CreateTmpVarDecl(nullptr, std::move(methodCall));
    methodCallRes->SetTy(retActualTy);
    OwnedPtr<Expr> retExpr;
    auto createCJMappingCall = [&library = this->lib, &jniEnvPtrParam, &curFile, &methodCallRes, &retExpr](
                                   std::string& clazzName, bool needCtorArgs) {
        std::replace(clazzName.begin(), clazzName.end(), '.', '/');
        auto entity = library.CreateCFFINewJavaCFFINewJavaProxyObjectForCJMappingCall(
            WithinFile(CreateRefExpr(jniEnvPtrParam), curFile), WithinFile(CreateRefExpr(*methodCallRes), curFile),
            clazzName, needCtorArgs);
        retExpr = WithinFile(std::move(entity), curFile);
    };

    if (retActualTy->IsPrimitive()) {
        retExpr = WithinFile(CreateRefExpr(*methodCallRes), curFile);
    } else if (IsCJMapping(*retActualTy)) {
        if (auto retStructTy = DynamicCast<StructTy*>(retActualTy)) {
            std::string clazzName = retStructTy->decl->fullPackageName + "." + retActualTy->name;
            createCJMappingCall(clazzName, true);
        } else if (auto retEnumTy = DynamicCast<EnumTy*>(retActualTy)) {
            std::string clazzName = retEnumTy->decl->fullPackageName + "." + retActualTy->name;
            createCJMappingCall(clazzName, false);
        } else if (auto retClassTy = DynamicCast<ClassTy*>(retActualTy)) {
            std::string clazzName = retClassTy->decl->fullPackageName + "." + retActualTy->name;
            createCJMappingCall(clazzName, true);
        }
    } else if (retActualTy->IsTuple()) {
        if (IsCJMappingTuple(retActualTy, tupleConfigs)) {
            std::string clazzName = decl.fullPackageName + "." + GetCjMappingTupleName(*retActualTy);
            createCJMappingCall(clazzName, true);
        } else {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_cjmapping_method_ret_unsupported, sampleMethod,
                retActualTy->String(), "cangjie mirror decl");
            sampleMethod.EnableAttr(Attribute::IS_BROKEN);
            sampleMethod.outerDecl->EnableAttr(Attribute::HAS_BROKEN);
            sampleMethod.outerDecl->EnableAttr(Attribute::IS_BROKEN);
            return nullptr;
        }
    } else if (retActualTy->IsString()) {
		// Convert Cangjie String to JNI jobject (jstring):
		// JNI boundary: jstring (represented as jobject / CPointer<Unit>)
        retExpr = lib.StringToJObject(CreateRefExpr(*methodCallRes), curFile, jniEnvPtrParam, *sampleMethod.outerDecl);
        retActualTy = lib.GetJobjectTy();
    } else if (retActualTy->IsFunc()) {
        CheckCjLambdaDeclByTy(retActualTy);
        std::string lambdaJavaClassSign =
            NormalizeJavaSignature(sampleMethod.fullPackageName + "." + GetLambdaJavaClassName(retActualTy) + "$Box");
        auto refExpr = WithinFile(CreateRefExpr(*methodCallRes), curFile);
        retExpr = lib.CreateGetJavaLambdaObjectCall(std::move(refExpr), lambdaJavaClassSign, curFile);
    } else if (IsOptionOfString(retActualTy)) {
        retExpr = lib.OptionStringToJObject(WithinFile(CreateRefExpr(*methodCallRes), curFile),
                                            jniEnvPtrParam,
                                            *sampleMethod.outerDecl);
        retActualTy = lib.GetJobjectTy();
    } else {
        OwnedPtr<Expr> methodResRef = WithinFile(CreateRefExpr(*methodCallRes), curFile);
        auto entity = lib.WrapJavaEntity(std::move(methodResRef));
        retExpr = lib.UnwrapJavaEntity(std::move(entity), retActualTy, *(sampleMethod.outerDecl), true);
    }

    auto wrappedNodesLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(methodCallRes), std::move(retExpr)));
    auto funcName = GetJniMethodName(sampleMethod);
    if (genericConfig && !genericConfig->declInstName.empty()) {
        funcName = GetJniMethodName(sampleMethod, &genericConfig->declInstName);
    }

    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    return GenerateNativeFuncDeclBylambda(decl, wrappedNodesLambda, paramLists, jniEnvPtrParam, retActualTy, funcName);
}

void JavaDesugarManager::GenerateFuncParamsForNativeDeleteCjObject(
    Decl& decl, std::vector<OwnedPtr<FuncParam>>& params, FuncParam*& jniEnv, OwnedPtr<Expr>& selfRef)
{
    auto& jniEnvParam = PushEnvParams(params);
    PushObjParams(params);
    auto& registryIdParam = PushSelfParams(params);
    CJC_ASSERT_WITH_MSG(!params.empty(), "jniEnv is absent");
    jniEnv = &jniEnvParam;
    CJC_NULLPTR_CHECK(decl.curFile);
    selfRef = WithinFile(CreateRefExpr(registryIdParam), decl.curFile);
}

OwnedPtr<Decl> JavaDesugarManager::GenerateNativeFuncDeclBylambda(Decl& decl, OwnedPtr<LambdaExpr>& wrappedNodesLambda,
    std::vector<OwnedPtr<FuncParamList>>& paramLists, FuncParam& jniEnvPtrParam, Ptr<Ty>& retTy, std::string funcName)
{
    CJC_NULLPTR_CHECK(decl.curFile);
    return GenerateNativeFuncDeclBylambda(wrappedNodesLambda, paramLists, jniEnvPtrParam, retTy, funcName, decl.curFile,
        decl.moduleName, decl.fullPackageName);
}

OwnedPtr<Decl> JavaDesugarManager::GenerateNativeFuncDeclBylambda(OwnedPtr<LambdaExpr>& wrappedNodesLambda,
    std::vector<OwnedPtr<FuncParamList>>& paramLists, FuncParam& jniEnvPtrParam, Ptr<Ty>& retTy, std::string funcName,
    Ptr<File>& curFile, std::string moduleName, std::string fullPackageName)
{
    //  For ty is CJMapping:
    //  when ty is ArgsTy, we could use the Java_CFFI_getFromRegistry with [id: jlong] to get the cangjie side
    //  struct/class. when ty is RetTy, just use [jobjectTy] for we need JNI to construct the ret object.
    Ptr<Ty> jniRetTy =
        IsCJMapping(*retTy) || IsCJMappingTuple(retTy, tupleConfigs) ? lib.GetJobjectTy() : GetJNITy(retTy);

    CJC_ASSERT(!paramLists.empty());
    CJC_NULLPTR_CHECK(curFile);
    auto fdecl = utils.CreateNativeFunc(funcName,
        std::move(paramLists[0]->params), jniRetTy, {}, *curFile, moduleName, fullPackageName);

    auto catchingCall =
        lib.WrapExceptionHandling(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile), std::move(wrappedNodesLambda));
    fdecl->funcBody->body->body.emplace_back(std::move(catchingCall));
    return std::move(fdecl);
}

/**
 * when isClassLikeDecl is true: argument ctor: generated constructor mapped with Java_ClassName_initCJObject func
 * when isClassLikeDecl is false: argument ctor: origin constructor mapped with Java_ClassName_initCJObject func
 */
OwnedPtr<Decl> JavaDesugarManager::GenerateNativeInitCjObjectFunc(FuncDecl& ctor, bool isClassLikeDecl,
    bool isOpenClass, Ptr<FuncDecl> fwdCtor, const GenericConfigInfo* genericConfig)
{
    if (isClassLikeDecl) {
        CJC_ASSERT_WITH_MSG(!ctor.funcBody->paramLists.empty(), "paramLists cannot be empty");
        CJC_ASSERT(!ctor.funcBody->paramLists[0]->params.empty()); // it contains obj: JavaEntity as minimum
    }

    if (isOpenClass) {
        CJC_ASSERT(fwdCtor);
    }

    // func decl arguments construction
    std::vector<OwnedPtr<FuncParam>> params;
    std::vector<OwnedPtr<FuncArg>> ctorCallArgs;
    PushEnvParams(params);
    PushObjParams(params);

    auto curFile = ctor.curFile;
    CJC_NULLPTR_CHECK(curFile);
    CJC_ASSERT_WITH_MSG(params.size() > 1, "expected at least 2 params");
    auto& jniEnvPtrParam = *(params[0]);
    auto objParamRef = WithinFile(CreateRefExpr(*params[1]), curFile);
    if (isClassLikeDecl || isOpenClass) {
        auto objAsEntity = lib.CreateJavaEntityJobjectCall(std::move(objParamRef));
        auto objWeakRef = lib.CreateNewGlobalRefCall(
            WithinFile(CreateRefExpr(jniEnvPtrParam), curFile), std::move(objAsEntity), true);
        ctorCallArgs.push_back(CreateFuncArg(std::move(objWeakRef)));
    }

    if (isOpenClass) {
        auto overrideMaskParam =
            CreateFuncParam(JAVA_OVERRIDE_MASK_NAME, lib.CreateJlongType(), nullptr, lib.GetJlongTy());
        auto paramRef = WithinFile(CreateRefExpr(*overrideMaskParam), curFile);
        ctorCallArgs.push_back(CreateFuncArg(std::move(paramRef)));
        params.push_back(std::move(overrideMaskParam));
    }

    std::unordered_map<std::string, Ptr<Ty>> actualTyArgMap;
    std::vector<Ptr<Ty>> funcTyParams;
    std::vector<OwnedPtr<Type>> actualPrimitiveType;
    if (genericConfig && genericConfig->instTypes.size() != 0) {
        GetArgsAndRetGenericActualTyVector(
            genericConfig, ctor, actualTyArgMap, funcTyParams, actualPrimitiveType, typeManager);
    }
    CJC_ASSERT_WITH_MSG(!ctor.funcBody->paramLists.empty(), "paramLists cannot be empty");
    for (size_t argIdx = 0; argIdx < ctor.funcBody->paramLists[0]->params.size(); ++argIdx) {
        auto& arg = ctor.funcBody->paramLists[0]->params[argIdx];
        if (isClassLikeDecl && argIdx == 0) {
            continue;
        }

        auto genericInstTy = GetGenericInstTy(genericConfig, arg->GetTy(), typeManager);
        if (!FillMethodParamsByArg(params, ctorCallArgs, ctor, arg, jniEnvPtrParam, genericInstTy)) {
            return nullptr;
        }
    }

    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    auto jlongTy = lib.GetJlongTy();
    auto funcName = GetJniInitCjObjectFuncName(ctor, isClassLikeDecl);
    if (genericConfig && !genericConfig->declInstName.empty()) {
        funcName = GetJniInitCjObjectFuncName(ctor, isClassLikeDecl, &genericConfig->declInstName);
    }
    OwnedPtr<CallExpr> objectCtorCall;

    if (isOpenClass) {
        auto fwdCtorRef = WithinFile(CreateRefExpr(*fwdCtor), curFile);
        objectCtorCall = CreateCallExpr(std::move(fwdCtorRef), std::move(ctorCallArgs), fwdCtor,
            fwdCtor->outerDecl->GetTy(), CallKind::CALL_OBJECT_CREATION);
    } else if (ctor.outerDecl->GetTy()->HasGeneric() && ctor.outerDecl->astKind == ASTKind::ENUM_DECL) {
        auto enumDecl = StaticCast<EnumDecl*>(ctor.outerDecl);
        auto enumRefExpr = WithinFile(CreateRefExpr(*enumDecl), curFile);
        enumRefExpr->typeArguments = std::move(actualPrimitiveType);
        auto enumTy = GetInstantyForGenericTy(*ctor.outerDecl, actualTyArgMap, typeManager);
        enumRefExpr->SetTy(enumTy);
        auto retTy = StaticCast<FuncTy*>(ctor.GetTy())->retTy;
        Ptr<FuncTy> funcTy;
        if (retTy->HasGeneric()) {
            funcTy = typeManager.GetFunctionTy(funcTyParams, enumTy, {.isC = true});
        } else {
            funcTy = typeManager.GetFunctionTy(funcTyParams, retTy, {.isC = true});
        }
        OwnedPtr<MemberAccess> methodAccess = CreateMemberAccess(std::move(enumRefExpr), ctor);
        methodAccess->curFile = curFile;
        methodAccess->SetTy(funcTy);
        objectCtorCall = CreateCallExpr(
            std::move(methodAccess), std::move(ctorCallArgs), Ptr(&ctor), enumTy, CallKind::CALL_OBJECT_CREATION);
    } else if (ctor.outerDecl->GetTy()->HasGeneric()) {
        auto instantiationRefExpr = CreateRefExpr(ctor);
        auto retTy = StaticCast<FuncTy*>(ctor.GetTy())->retTy;
        Ptr<FuncTy> funcTy;
        auto instantTy = GetInstantyForGenericTy(*ctor.outerDecl, actualTyArgMap, typeManager);
        if (retTy->HasGeneric()) {
            funcTy = typeManager.GetFunctionTy(funcTyParams, instantTy, {.isC = true});
        } else {
            funcTy = typeManager.GetFunctionTy(funcTyParams, retTy, {.isC = true});
        }
        instantiationRefExpr->typeArguments = std::move(actualPrimitiveType);
        instantiationRefExpr->SetTy(funcTy);
        objectCtorCall = CreateCallExpr(WithinFile(std::move(instantiationRefExpr), curFile), std::move(ctorCallArgs),
            Ptr(&ctor), instantTy, CallKind::CALL_OBJECT_CREATION);
    } else {
        auto ctorRef = WithinFile(CreateRefExpr(ctor), curFile);
        objectCtorCall = CreateCallExpr(std::move(ctorRef), std::move(ctorCallArgs), Ptr(&ctor),
            ctor.outerDecl->GetTy(), CallKind::CALL_OBJECT_CREATION);
    }

    auto putToRegistryCall = lib.CreatePutToRegistryCall(std::move(objectCtorCall));
    auto bodyLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(putToRegistryCall)));
    return GenerateNativeFuncDeclBylambda(ctor, bodyLambda, paramLists, jniEnvPtrParam, jlongTy, funcName);
}

OwnedPtr<Decl> JavaDesugarManager::GenerateNativeInitCjObjectFunc(const Ptr<TupleTy>& tupleTy, Package& pkg)
{
    auto curFile = pkg.files.at(0).get();
    CJC_NULLPTR_CHECK(curFile);

    std::vector<OwnedPtr<FuncParam>> params;
    std::vector<OwnedPtr<Expr>> tupleElements;
    PushEnvParams(params);
    PushObjParams(params);

    auto& jniEnvPtrParam = *(params[0]);
    std::vector<OwnedPtr<FuncParamList>> paramLists;

    size_t i = 0;
    for (const auto& it : tupleTy->typeArgs) {
        std::string paramName = "item" + std::to_string(i);
        ++i;
        OwnedPtr<FuncParam> param = CreateFuncParam(paramName, nullptr, nullptr, it);
        auto paramRef = WithinFile(CreateRefExpr(*param), curFile);
        params.push_back(std::move(param));
        tupleElements.push_back(std::move(paramRef));
    }

    paramLists.push_back(CreateFuncParamList(std::move(params)));
    auto tupleLit = WithinFile(CreateTupleLit(std::move(tupleElements), tupleTy), curFile);
    auto putToRegistryCall = lib.CreatePutToRegistryCall(std::move(tupleLit));
    auto bodyLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(putToRegistryCall)));
    auto jlongTy = lib.GetJlongTy();
    auto funcName = GetJniInitCjObjectFuncName(tupleTy, pkg);
    return GenerateNativeFuncDeclBylambda(bodyLambda, paramLists, jniEnvPtrParam, jlongTy, funcName, curFile,
        ::Cangjie::Utils::GetRootPackageName(pkg.fullPackageName), pkg.fullPackageName);
}

/**
 *  Map Cangjie type to corresponding JNI-level type used in generated native method.
 */
Ptr<Ty> JavaDesugarManager::GetJNITy(Ptr<Ty> ty)
{
    static auto jobjectTy = lib.GetJobjectTy();
    static auto jlongTy = lib.GetJlongTy();

    if (!ty) {
        return nullptr;
    }
    if (ty->IsString()) {
        // String is passed as jobject.
        return jobjectTy;
    }
    if (ty->IsCoreOptionType()) {
        return jobjectTy;
    }
    if (IsMirror(*ty) || IsImpl(*ty) || IsCJMappingInterface(*ty) || ty->IsFunc()) {
        return jobjectTy;
    }
    if (IsCJMapping(*ty)) {
        return jlongTy;
    }
    if (ty->IsTuple()) {
        return jlongTy;
    }
    if (ty->kind == TypeKind::TYPE_GENERICS) {
        return nullptr;
    }
    CJC_ASSERT(ty->IsBuiltin());
    return ty;
}

std::string JavaDesugarManager::GetJniMethodName(const FuncDecl& method, const std::string* genericActualName)
{
    auto sampleJavaName = GetJavaMemberName(method);
    std::string fqname = GetJavaFQName(*(method.outerDecl), genericActualName);
    MangleJNIName(fqname);
    CJC_ASSERT_WITH_MSG(!method.funcBody->paramLists.empty(), "paramLists cannot be empty");
    auto mangledFuncName =
        GetMangledMethodName(mangler, method.funcBody->paramLists[0]->params, sampleJavaName, typeManager);
    MangleJNIName(mangledFuncName);

    return "Java_" + fqname + "_" + mangledFuncName;
}

std::string JavaDesugarManager::GetJniTupleItemName(const Ptr<TupleTy>& tupleTy, Package& pkg, size_t index)
{
    std::string fqname = pkg.fullPackageName + "." + GetCjMappingTupleName(*tupleTy);
    MangleJNIName(fqname);

    return "Java_" + fqname + "_" + "item" + std::to_string(index);
}

std::string JavaDesugarManager::GetJniMethodNameForProp(
    const PropDecl& propDecl, bool isSet, const std::string* genericActualName) const
{
    std::string varDecl = GetJavaMemberName(propDecl);
    MangleJNIName(varDecl);
    std::string varDeclSuffix = varDecl;
    CJC_ASSERT_WITH_MSG(!varDeclSuffix.empty(), "identifier cannot be an empty string");
    varDeclSuffix[0] = static_cast<char>(toupper(varDeclSuffix[0]));
    std::string fqname = GetJavaFQName(*(propDecl.outerDecl), genericActualName);
    MangleJNIName(fqname);
    return "Java_" + fqname + (isSet ? "_set" : "_get") + varDeclSuffix + "Impl";
    ;
}

std::string JavaDesugarManager::GetJniInitCjObjectFuncName(
    const FuncDecl& ctor, bool isGeneratedCtor, const std::string* genericActualName)
{
    std::string fqname = GetJavaFQName(*(ctor.outerDecl), genericActualName);
    MangleJNIName(fqname);
    auto mangledFuncName =
        GetMangledJniInitCjObjectFuncName(mangler, ctor.funcBody->paramLists[0]->params, isGeneratedCtor);
    MangleJNIName(mangledFuncName);

    if (Is<EnumDecl>(ctor.outerDecl)) {
        mangledFuncName = ctor.identifier + mangledFuncName;
    }

    return "Java_" + fqname + "_" + mangledFuncName;
}

std::string JavaDesugarManager::GetJniInitCjObjectFuncName(const Ptr<TupleTy>& tupleTy, Package& pkg)
{
    std::string fqname = pkg.fullPackageName + "." + GetCjMappingTupleName(*tupleTy);
    MangleJNIName(fqname);
    std::string mangledFuncName = GetMangledJniInitCjObjectFuncName(mangler, tupleTy->typeArgs);
    MangleJNIName(mangledFuncName);
    return "Java_" + fqname + "_" + mangledFuncName;
}

std::string JavaDesugarManager::GetJniInitCjObjectFuncNameForVarDecl(const AST::VarDecl& ctor) const
{
    std::string fqname = GetJavaFQName(*(ctor.outerDecl));
    MangleJNIName(fqname);
    auto mangledFuncName = ctor.identifier.Val();
    MangleJNIName(mangledFuncName);
    return "Java_" + fqname + "_" + mangledFuncName + "initCJObject";
}

std::string JavaDesugarManager::GetJniDetachCjObjectFuncName(const Decl& decl) const
{
    std::string fqname = GetJavaFQName(decl);
    MangleJNIName(fqname);

    return "Java_" + fqname + "_detachCJObject";
}

std::string JavaDesugarManager::GetJniDeleteCjObjectFuncName(const Decl& decl) const
{
    std::string fqname = GetJavaFQName(decl);
    MangleJNIName(fqname);

    return "Java_" + fqname + "_deleteCJObject";
}

} // namespace Cangjie::Interop::Java
