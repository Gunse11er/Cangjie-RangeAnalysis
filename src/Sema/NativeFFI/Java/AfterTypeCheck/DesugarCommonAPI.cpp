// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "NativeFFI/Java/JavaCodeGenerator/JavaSourceCodeGenerator.h"
#include "Utils.h"

#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"

namespace Cangjie::Interop::Java {
using namespace Cangjie::Native::FFI;

inline void JavaDesugarManager::PushEnvParams(std::vector<OwnedPtr<FuncParam>>& params, std::string name)
{
    auto jniEnvPtrDecl = lib.GetJniEnvPtrDecl();
    CJC_NULLPTR_CHECK(jniEnvPtrDecl);
    params.push_back(CreateFuncParam(name, ASTCloner::Clone(jniEnvPtrDecl->type.get()), nullptr, lib.GetJNIEnvPtrTy()));
}

inline void JavaDesugarManager::PushObjParams(std::vector<OwnedPtr<FuncParam>>& params, std::string name)
{
    params.push_back(CreateFuncParam(name, lib.CreateJobjectType(), nullptr, lib.GetJobjectTy()));
}

inline void JavaDesugarManager::PushSelfParams(std::vector<OwnedPtr<FuncParam>>& params, std::string name)
{
    params.push_back(CreateFuncParam(name, lib.CreateJlongType(), nullptr, lib.GetJlongTy()));
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

    auto fwdTy = fwdClassDecl.ty;
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
    Ptr<Ty> actualArgTy = arg->ty;
    actualArgTy = (actualTy && arg->ty->HasGeneric()) ? actualTy : arg->ty;
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
    } else if (actualArgTy->IsCoreOptionType() && IsMirror(*actualArgTy->typeArgs[0])) {
        // funcDecl(Java_CFFI_JavaEntity(arg)) // if arg is null (as jobject == 0) -> java entity will preserve it
        auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
        methodArg = CreateFuncArg(lib.UnwrapJavaEntity(std::move(entity), actualArgTy, *outerDecl));
    } else if (actualArgTy->IsCoreOptionType() && IsImpl(*actualArgTy->typeArgs[0])) {
        auto entity = lib.CreateJavaEntityJobjectCall(std::move(paramRef));
        methodArg = CreateFuncArg(lib.UnwrapJavaEntity(std::move(entity), actualArgTy, *outerDecl));
    } else if (IsCJMapping(*actualArgTy)) {
        auto entity = lib.CreateGetFromRegistryCall(
            WithinFile(CreateRefExpr(jniEnvPtrParam), funcDecl.curFile), std::move(paramRef), actualArgTy);
        methodArg = CreateFuncArg(WithinFile(std::move(entity), funcDecl.curFile));
    } else if (IsCJMappingInterface(*arg->ty)) {
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

    Ptr<Ty> retTy = StaticCast<FuncTy*>(sampleMethod.ty)->retTy;
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
        if (!FillMethodParamsByArg(params, methodCallArgs, sampleMethod, arg, jniEnvPtrParam,
            GetGenericInstTy(genericConfig, arg->ty, typeManager))) {
                return nullptr;
        }
    }
    OwnedPtr<MemberAccess> methodAccess;
    if (sampleMethod.TestAttr(Attribute::STATIC)) {
        auto staticRefExpr = CreateRefExpr(decl);
        if (decl.ty->HasGeneric()) {
            staticRefExpr->typeArguments = std::move(actualPrimitiveType);
            staticRefExpr->ty = instantTy;
        }
        methodAccess = CreateMemberAccess(WithinFile(std::move(staticRefExpr), curFile), sampleMethod);
    } else if (sampleMethod.TestAttr(Attribute::CJ_MIRROR_JAVA_INTERFACE_DEFAULT)) {
        auto& objParam = *params[2];
        auto paramRef = WithinFile(CreateRefExpr(objParam), curFile);
        auto fwdClassInstance = GetFwdClassInstance(std::move(paramRef), decl);
        methodAccess = CreateMemberAccess(std::move(fwdClassInstance), sampleMethod);
    } else {
        OwnedPtr<CallExpr> reg;
        if (decl.ty->HasGeneric()) {
            reg = lib.CreateGetFromRegistryCall(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile),
                WithinFile(CreateRefExpr(selfParam), curFile), instantTy);
            methodAccess = CreateMemberAccess(std::move(reg), sampleMethod);
            methodAccess->ty = typeManager.GetFunctionTy(funcTyParams, retActualTy);
        } else {
            reg = lib.CreateGetFromRegistryCall(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile),
                WithinFile(CreateRefExpr(selfParam), curFile), decl.ty);
            methodAccess = CreateMemberAccess(std::move(reg), sampleMethod);
        }
    }
    methodAccess->curFile = curFile;
    auto methodCall = CreateCallExpr(std::move(methodAccess), std::move(methodCallArgs), Ptr(&sampleMethod),
        retActualTy, CallKind::CALL_DECLARED_FUNCTION);
    auto methodCallRes = CreateTmpVarDecl(nullptr, std::move(methodCall));
    methodCallRes->ty = retActualTy;
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
    } else if (retActualTy->IsFunc()) {
        CheckCjLambdaDeclByTy(retActualTy);
        std::string lambdaJavaClassSign =
            NormalizeJavaSignature(sampleMethod.fullPackageName + "." + GetLambdaJavaClassName(retActualTy) + "$Box");
        auto refExpr = WithinFile(CreateRefExpr(*methodCallRes), curFile);
        retExpr = lib.CreateGetJavaLambdaObjectCall(std::move(refExpr), lambdaJavaClassSign, curFile);
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
    PushEnvParams(params);
    PushObjParams(params);
    PushSelfParams(params);
    CJC_ASSERT_WITH_MSG(!params.empty(), "jniEnv is absent");
    jniEnv = &(*params[0]);
    CJC_NULLPTR_CHECK(decl.curFile);
    constexpr int SELF_REF_INDEX = 2;
    CJC_ASSERT_WITH_MSG(params.size() > SELF_REF_INDEX, "selfRef is absent");
    selfRef = WithinFile(CreateRefExpr(*params[SELF_REF_INDEX]), decl.curFile);
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
    auto catchingCall =
        lib.WrapExceptionHandling(WithinFile(CreateRefExpr(jniEnvPtrParam), curFile), std::move(wrappedNodesLambda));
    //  For ty is CJMapping:
    //  when ty is ArgsTy, we could use the Java_CFFI_getFromRegistry with [id: jlong] to get the cangjie side
    //  struct/class. when ty is RetTy, just use [jobjectTy] for we need JNI to construct the ret object.
    Ptr<Ty> jniRetTy =
        IsCJMapping(*retTy) || IsCJMappingTuple(retTy, tupleConfigs) ? lib.GetJobjectTy() : GetJNITy(retTy);
    auto block = CreateBlock(Nodes(std::move(catchingCall)), jniRetTy);
    auto funcBody = CreateFuncBody(std::move(paramLists), nullptr, std::move(block), jniRetTy);
    std::vector<Ptr<Ty>> funcTyParams;
    CJC_ASSERT_WITH_MSG(!funcBody->paramLists.empty(), "paramLists cannot be empty");
    for (auto& param : funcBody->paramLists[0]->params) {
        funcTyParams.push_back(param->ty);
    }
    auto funcTy = typeManager.GetFunctionTy(funcTyParams, jniRetTy, {.isC = true});
    auto fdecl = CreateFuncDecl(funcName, std::move(funcBody), funcTy);
    fdecl->funcBody->funcDecl = fdecl.get();
    fdecl->EnableAttr(Attribute::C);
    fdecl->EnableAttr(Attribute::GLOBAL);
    fdecl->EnableAttr(Attribute::PUBLIC);
    fdecl->EnableAttr(Attribute::NO_MANGLE);
    fdecl->EnableAttr(Attribute::UNSAFE);
    fdecl->curFile = curFile;
    fdecl->moduleName = moduleName;
    fdecl->fullPackageName = fullPackageName;

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

        if (!FillMethodParamsByArg(params, ctorCallArgs, ctor, arg, jniEnvPtrParam,
            GetGenericInstTy(genericConfig, arg->ty, typeManager))) {
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
        objectCtorCall = CreateCallExpr(std::move(fwdCtorRef), std::move(ctorCallArgs), fwdCtor, fwdCtor->outerDecl->ty,
            CallKind::CALL_OBJECT_CREATION);
    } else if (ctor.outerDecl->ty->HasGeneric() && ctor.outerDecl->astKind == ASTKind::ENUM_DECL) {
        auto enumDecl = StaticCast<EnumDecl*>(ctor.outerDecl);
        auto enumRefExpr = WithinFile(CreateRefExpr(*enumDecl), curFile);
        enumRefExpr->typeArguments = std::move(actualPrimitiveType);
        auto enumTy = GetInstantyForGenericTy(*ctor.outerDecl, actualTyArgMap, typeManager);
        enumRefExpr->ty = enumTy;
        auto retTy = StaticCast<FuncTy*>(ctor.ty)->retTy;
        Ptr<FuncTy> funcTy;
        if (retTy->HasGeneric()) {
            funcTy = typeManager.GetFunctionTy(funcTyParams, enumTy, {.isC = true});
        } else {
            funcTy = typeManager.GetFunctionTy(funcTyParams, retTy, {.isC = true});
        }
        OwnedPtr<MemberAccess> methodAccess = CreateMemberAccess(std::move(enumRefExpr), ctor);
        methodAccess->curFile = curFile;
        methodAccess->ty = funcTy;
        objectCtorCall = CreateCallExpr(
            std::move(methodAccess), std::move(ctorCallArgs), Ptr(&ctor), enumTy, CallKind::CALL_OBJECT_CREATION);
    } else if (ctor.outerDecl->ty->HasGeneric()) {
        auto instantiationRefExpr = CreateRefExpr(ctor);
        auto retTy = StaticCast<FuncTy*>(ctor.ty)->retTy;
        Ptr<FuncTy> funcTy;
        auto instantTy = GetInstantyForGenericTy(*ctor.outerDecl, actualTyArgMap, typeManager);
        if (retTy->HasGeneric()) {
            funcTy = typeManager.GetFunctionTy(funcTyParams, instantTy, {.isC = true});
        } else {
            funcTy = typeManager.GetFunctionTy(funcTyParams, retTy, {.isC = true});
        }
        instantiationRefExpr->typeArguments = std::move(actualPrimitiveType);
        instantiationRefExpr->ty = funcTy;
        objectCtorCall = CreateCallExpr(WithinFile(std::move(instantiationRefExpr), curFile), std::move(ctorCallArgs),
            Ptr(&ctor), instantTy, CallKind::CALL_OBJECT_CREATION);
    } else {
        auto ctorRef = WithinFile(CreateRefExpr(ctor), curFile);
        objectCtorCall = CreateCallExpr(std::move(ctorRef), std::move(ctorCallArgs), Ptr(&ctor), ctor.outerDecl->ty,
            CallKind::CALL_OBJECT_CREATION);
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

} // namespace Cangjie::Interop::Java
