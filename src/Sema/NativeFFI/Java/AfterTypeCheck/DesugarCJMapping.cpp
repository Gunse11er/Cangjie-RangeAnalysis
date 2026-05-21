// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JavaDesugarManager.h"
#include "NativeFFI/Java/JavaCodeGenerator/JavaSourceCodeGenerator.h"
#include "Utils.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Symbol.h"

namespace Cangjie::Interop::Java {
using namespace Cangjie::Native::FFI;

Ptr<VarDecl> GetFwdClassField(ClassDecl& fwdDecl, std::string identifier)
{
    for (auto& member : fwdDecl.body->decls) {
        if (auto varDecl = As<ASTKind::VAR_DECL>(member); varDecl && varDecl->identifier == identifier) {
            return varDecl;
        }
    }
    return nullptr;
}

Ptr<FuncDecl> GetFwdClassMethod(ClassDecl& fwdDecl, std::string identifier)
{
    for (auto& member : fwdDecl.body->decls) {
        if (auto funcDecl = As<ASTKind::FUNC_DECL>(member); funcDecl && funcDecl->identifier == identifier) {
            return funcDecl;
        }
    }
    return nullptr;
}

// Support Struct decl and Enum decl for now.
OwnedPtr<Decl> JavaDesugarManager::GenerateCJMappingNativeDeleteCjObjectFunc(Decl& decl)
{
    std::vector<OwnedPtr<FuncParam>> params;
    FuncParam* jniEnvPtrParam = nullptr;
    OwnedPtr<Expr> selfParamRef;
    GenerateFuncParamsForNativeDeleteCjObject(decl, params, jniEnvPtrParam, selfParamRef);
    CJC_NULLPTR_CHECK(jniEnvPtrParam);

    auto removeFromRegistryCall = lib.CreateRemoveFromRegistryCall(std::move(selfParamRef));
    auto wrappedNodesLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(removeFromRegistryCall)));
    Ptr<Ty> unitTy = typeManager.GetPrimitiveTy(TypeKind::TYPE_UNIT).get();
    auto funcName = GetJniDeleteCjObjectFuncName(decl);
    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    return GenerateNativeFuncDeclBylambda(decl, wrappedNodesLambda, paramLists, *jniEnvPtrParam, unitTy, funcName);
}

OwnedPtr<Decl> JavaDesugarManager::GenerateCJMappingNativeDetachCjObjectFunc(ClassDecl& fwdDecl, ClassDecl& classDecl)
{
    std::vector<OwnedPtr<FuncParam>> params;
    FuncParam* jniEnvPtrParam = nullptr;
    OwnedPtr<Expr> selfParamRef;
    GenerateFuncParamsForNativeDeleteCjObject(fwdDecl, params, jniEnvPtrParam, selfParamRef);
    constexpr int selfRefIndex = 2;
    constexpr int objRefIndex = 1;
    OwnedPtr<Expr> envParamRef = WithinFile(CreateRefExpr(*jniEnvPtrParam), fwdDecl.curFile);
    OwnedPtr<Expr> objParamRef = WithinFile(CreateRefExpr(*params[objRefIndex]), fwdDecl.curFile);

    auto javaEntityCall = lib.CreateJavaEntityJobjectCall(std::move(objParamRef));
    auto reg = lib.CreateGetFromRegistryCall(std::move(envParamRef), std::move(selfParamRef), fwdDecl.ty);
    auto controllerVar = GetFwdClassField(fwdDecl, JAVA_OBJECT_CONTROLLER_NAME);
    auto varAccess = CreateMemberAccess(std::move(reg), *controllerVar);
    auto detachCjObjectFd = lib.GetDetachCJObjectDecl();
    auto retTy = StaticCast<FuncTy*>(detachCjObjectFd->ty)->retTy;
    auto funcAccess = CreateMemberAccess(std::move(varAccess), *detachCjObjectFd);

    std::vector<OwnedPtr<FuncArg>> args;
    OwnedPtr<Expr> selfParamRefTmp = WithinFile(CreateRefExpr(*params[selfRefIndex]), fwdDecl.curFile);
    OwnedPtr<Expr> envParamRefTmp = WithinFile(CreateRefExpr(*jniEnvPtrParam), fwdDecl.curFile);
    OwnedPtr<Expr> objParamRefTmp = WithinFile(CreateRefExpr(*params[objRefIndex]), fwdDecl.curFile);
    args.push_back(CreateFuncArg(std::move(envParamRefTmp)));
    args.push_back(CreateFuncArg(std::move(objParamRefTmp)));
    args.push_back(CreateFuncArg(std::move(selfParamRefTmp)));

    auto methodCall = CreateCallExpr(
        std::move(funcAccess), std::move(args), detachCjObjectFd, retTy, CallKind::CALL_DECLARED_FUNCTION);

    auto wrappedNodesLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(methodCall)));
    Ptr<Ty> unitTy = typeManager.GetPrimitiveTy(TypeKind::TYPE_UNIT).get();
    auto funcName = GetJniDetachCjObjectFuncName(classDecl);
    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    return GenerateNativeFuncDeclBylambda(fwdDecl, wrappedNodesLambda, paramLists, *jniEnvPtrParam, unitTy, funcName);
}

// Current support struct, class type.
void JavaDesugarManager::GenerateForCJStructOrClassTypeMapping(const File& file, AST::Decl* decl)
{
    CJC_ASSERT(decl && IsCJMapping(*decl));
    auto classDecl = DynamicCast<AST::ClassDecl*>(decl);
    auto structDecl = DynamicCast<AST::StructDecl*>(decl);
    CJC_ASSERT((classDecl || structDecl) && "Not a support ref type.");
    std::vector<FuncDecl*> generatedCtors;

    std::vector<GenericConfigInfo*> genericConfigsVector;
    bool isGenericGlueCode = false;
    InitGenericConfigs(file, decl, genericConfigsVector, isGenericGlueCode);
    for (auto& member : decl->GetMemberDecls()) {
        if (member->TestAnyAttr(Attribute::IS_BROKEN, Attribute::PRIVATE, Attribute::PROTECTED, Attribute::INTERNAL) ||
            (file.curPackage.get()->isInteropCJPackageConfig && member.get()->symbol &&
                !member.get()->symbol->isNeedExposedToInterop)) {
            continue;
        }
        if (auto fd = As<ASTKind::FUNC_DECL>(member.get())) {
            if (fd->TestAttr(Attribute::CONSTRUCTOR)) {
                generatedCtors.push_back(fd);
            } else {
                if (isGenericGlueCode) {
                    for (auto genericConfig : genericConfigsVector) {
                        auto nativeMethod = GenerateNativeMethod(*fd, *decl, genericConfig);
                        if (nativeMethod != nullptr) {
                            generatedDecls.push_back(std::move(nativeMethod));
                        }
                    }
                } else {
                    auto nativeMethod = GenerateNativeMethod(*fd, *decl);
                    if (nativeMethod != nullptr) {
                        generatedDecls.push_back(std::move(nativeMethod));
                    }
                }
            }
        }
    }
    if (!generatedCtors.empty()) {
        generatedDecls.push_back(GenerateCJMappingNativeDeleteCjObjectFunc(*decl));
        for (auto generatedCtor : generatedCtors) {
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    generatedDecls.push_back(
                        GenerateNativeInitCjObjectFunc(*generatedCtor, false, false, nullptr, genericConfig));
                }
            } else {
                generatedDecls.push_back(GenerateNativeInitCjObjectFunc(*generatedCtor, false));
            }
        }
    }
}

void JavaDesugarManager::GenerateTuplesGlueCode(Package& pkg)
{
    for (const auto& it : pkg.interopTuples) {
        std::string result = it;
        if (!result.empty() && result.front() == '(') {
            result.erase(0, 1);
        }
        if (!result.empty() && result.back() == ')') {
            result.erase(result.size() - 1, 1);
        }

        std::vector<std::string> actualTypes;
        SplitAndTrim(result, actualTypes);
        std::vector<Ptr<Ty>> elements;
        for (const auto& typeStr : actualTypes) {
            auto ty = GetTyByName(typeStr);
            CJC_ASSERT(Ty::IsTyCorrect(ty));
            elements.push_back(ty);
        }
        auto tupleTy = typeManager.GetTupleTy(elements);
        CJC_ASSERT(Ty::IsTyCorrect(tupleTy));

        if (!tupleConfigs.insert(tupleTy).second) {
            continue;
        }

        generatedDecls.push_back(GenerateNativeInitCjObjectFunc(tupleTy, pkg));
        GenerateNativeItemFunc(tupleTy, pkg);
        auto helperDecl = CreateHelperStructDecl(tupleTy, pkg);
        generatedDecls.push_back(GenerateCJMappingNativeDeleteCjObjectFunc(*helperDecl));
        const std::string fileJ = GetCjMappingTupleName(*tupleTy) + ".java";
        auto codegen = JavaSourceCodeGenerator(helperDecl.get(), mangler, typeManager, javaCodeGenPath, fileJ,
            GetCangjieLibName(outputLibPath, helperDecl.get()->GetFullPackageName()), tupleTy, true,
            pkg.isInteropCJPackageConfig);
        codegen.Generate();
    }
}

OwnedPtr<StructDecl> JavaDesugarManager::CreateHelperStructDecl(const Ptr<TupleTy>& tupleTy, Package& pkg)
{
    auto curFile = pkg.files.at(0).get();
    CJC_NULLPTR_CHECK(curFile);

    auto helperDecl = MakeOwned<StructDecl>();
    helperDecl->identifier = GetCjMappingTupleName(*tupleTy);
    helperDecl->fullPackageName = pkg.fullPackageName;
    helperDecl->moduleName = ::Cangjie::Utils::GetRootPackageName(pkg.fullPackageName);
    helperDecl->curFile = curFile;
    helperDecl->EnableAttr(Attribute::PUBLIC, Attribute::COMPILER_ADD, Attribute::JAVA_CJ_MAPPING);
    helperDecl->body = MakeOwned<StructBody>();

    std::vector<Ptr<Ty>> typeArgs;
    helperDecl->ty = typeManager.GetStructTy(*helperDecl, typeArgs);
    std::vector<OwnedPtr<FuncParam>> params;
    size_t i = 0;
    for (const auto& it : tupleTy->typeArgs) {
        std::string itemName = "item" + std::to_string(i);
        ++i;
        auto paramType = MakeOwned<Type>();
        paramType->ty = it;
        OwnedPtr<FuncParam> param = CreateFuncParam(itemName, std::move(paramType), nullptr, it);
        params.push_back(std::move(param));
    }

    auto retTy = helperDecl->ty;
    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));
    auto funcBody = CreateFuncBody(std::move(paramLists), nullptr, nullptr, retTy);
    std::vector<Ptr<Ty>> funcTyParams;
    for (auto& param : funcBody->paramLists[0]->params) {
        funcTyParams.push_back(param->ty);
    }
    auto funcTy = typeManager.GetFunctionTy(funcTyParams, retTy);
    auto fdecl = CreateFuncDecl("init", std::move(funcBody), funcTy);
    fdecl->funcBody->funcDecl = fdecl.get();
    fdecl->EnableAttr(Attribute::PUBLIC);
    fdecl->EnableAttr(Attribute::CONSTRUCTOR);
    fdecl->EnableAttr(Attribute::ABSTRACT);
    fdecl->curFile = helperDecl->curFile;
    fdecl->moduleName = helperDecl->moduleName;
    fdecl->fullPackageName = helperDecl->fullPackageName;

    helperDecl->body->decls.push_back(std::move(fdecl));
    return std::move(helperDecl);
}

void JavaDesugarManager::GenerateNativeItemFunc(const Ptr<TupleTy>& tupleTy, Package& pkg)
{
    auto curFile = pkg.files.at(0).get();
    CJC_NULLPTR_CHECK(curFile);
    size_t i = 0;
    for (const auto& ty : tupleTy->typeArgs) {
        Ptr<Ty> retTy = ty;
        std::vector<OwnedPtr<FuncParam>> params;
        PushEnvParams(params);
        // jobject or jclass
        PushObjParams(params, "_");
        auto& jniEnvPtrParam = *params[0];
        PushSelfParams(params);

        auto& selfParam = *params.back();

        OwnedPtr<SubscriptExpr> tupleAccess;
        OwnedPtr<CallExpr> reg;
        reg = lib.CreateGetFromRegistryCall(
            WithinFile(CreateRefExpr(jniEnvPtrParam), curFile), WithinFile(CreateRefExpr(selfParam), curFile), tupleTy);
        tupleAccess = CreateTupleAccess(std::move(reg), i);

        tupleAccess->curFile = curFile;
        tupleAccess->indexExprs.at(0)->ty = TypeManager::GetPrimitiveTy(TypeKind::TYPE_INT64);
        auto tupleAccessRes = CreateTmpVarDecl(nullptr, std::move(tupleAccess));
        tupleAccessRes->ty = retTy;
        OwnedPtr<Expr> retExpr;

        retExpr = WithinFile(CreateRefExpr(*tupleAccessRes), curFile);

        auto wrappedNodesLambda =
            WrapReturningLambdaExpr(typeManager, Nodes(std::move(tupleAccessRes), std::move(retExpr)));
        auto funcName = GetJniTupleItemName(tupleTy, pkg, i);

        std::vector<OwnedPtr<FuncParamList>> paramLists;
        paramLists.push_back(CreateFuncParamList(std::move(params)));

        generatedDecls.push_back(GenerateNativeFuncDeclBylambda(wrappedNodesLambda, paramLists, jniEnvPtrParam, retTy,
            funcName, curFile, ::Cangjie::Utils::GetRootPackageName(pkg.fullPackageName), pkg.fullPackageName));
        ++i;
    }
}

OwnedPtr<Decl> JavaDesugarManager::GenerateNativeInitCjObjectFuncForEnumCtorNoParams(
    AST::EnumDecl& enumDecl, AST::VarDecl& ctor)
{
    // Empty params to build constructor from VarDecl.
    std::vector<OwnedPtr<FuncParam>> params;
    std::vector<OwnedPtr<FuncArg>> ctorCallArgs;
    PushEnvParams(params, "env");
    auto curFile = ctor.curFile;
    CJC_NULLPTR_CHECK(curFile);
    CJC_ASSERT_WITH_MSG(!params.empty(), "jniEnvPtrParam is absent");
    auto& jniEnvPtrParam = *(params[0]);

    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    auto enumRef = WithinFile(CreateRefExpr(enumDecl), curFile);
    auto objectCtorCall = CreateMemberAccess(std::move(enumRef), ctor.identifier);

    auto putToRegistryCall = lib.CreatePutToRegistryCall(std::move(objectCtorCall));
    auto bodyLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(putToRegistryCall)));
    auto jlongTy = lib.GetJlongTy();
    auto funcName = GetJniInitCjObjectFuncNameForVarDecl(ctor);
    return GenerateNativeFuncDeclBylambda(ctor, bodyLambda, paramLists, jniEnvPtrParam, jlongTy, funcName);
}

void JavaDesugarManager::GenerateNativeInitCJObjectEnumCtor(AST::EnumDecl& enumDecl)
{
    std::vector<GenericConfigInfo*> genericConfigsVector;
    bool isGenericGlueCode = false;
    InitGenericConfigs(*enumDecl.curFile, &enumDecl, genericConfigsVector, isGenericGlueCode);
    for (auto& ctor : enumDecl.constructors) {
        if (ctor->astKind == ASTKind::FUNC_DECL) {
            auto fd = As<ASTKind::FUNC_DECL>(ctor.get());
            CJC_NULLPTR_CHECK(fd);
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    generatedDecls.push_back(GenerateNativeInitCjObjectFunc(*fd, false, false, nullptr, genericConfig));
                }
            } else {
                generatedDecls.push_back(GenerateNativeInitCjObjectFunc(*fd, false));
            }
        } else if (ctor->astKind == ASTKind::VAR_DECL) {
            auto varDecl = As<ASTKind::VAR_DECL>(ctor.get());
            CJC_NULLPTR_CHECK(varDecl);
            generatedDecls.push_back(GenerateNativeInitCjObjectFuncForEnumCtorNoParams(enumDecl, *varDecl));
        }
    }
}

void JavaDesugarManager::GenerateForCJEnumMapping(AST::EnumDecl& enumDecl)
{
    CJC_ASSERT(IsCJMapping(enumDecl));

    GenerateNativeInitCJObjectEnumCtor(enumDecl);

    std::vector<GenericConfigInfo*> genericConfigsVector;
    bool isGenericGlueCode = false;
    InitGenericConfigs(*enumDecl.curFile, &enumDecl, genericConfigsVector, isGenericGlueCode);
    for (auto& member : enumDecl.GetMemberDecls()) {
        if (member->TestAttr(Attribute::IS_BROKEN) || !member->TestAttr(Attribute::PUBLIC)) {
            continue;
        }
        if (auto fd = As<ASTKind::FUNC_DECL>(member.get())) {
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    generatedDecls.push_back(GenerateNativeMethod(*fd, enumDecl, genericConfig));
                }
            } else {
                generatedDecls.push_back(GenerateNativeMethod(*fd, enumDecl));
            }
        } else if (member->astKind == ASTKind::PROP_DECL && !member->TestAttr(Attribute::COMPILER_ADD)) {
            const PropDecl& propDecl = *StaticAs<ASTKind::PROP_DECL>(member.get());
            CJC_ASSERT_WITH_MSG(!propDecl.getters.empty(), "property must have a getter");
            const OwnedPtr<FuncDecl>& funcDecl = propDecl.getters[0];
            if (isGenericGlueCode) {
                for (auto genericConfig : genericConfigsVector) {
                    auto getSignature = GetJniMethodNameForProp(propDecl, false);
                    if (genericConfig && !genericConfig->declInstName.empty()) {
                        getSignature = genericConfig
                            ? GetJniMethodNameForProp(propDecl, false, &genericConfig->declInstName)
                            : GetJniMethodNameForProp(propDecl, false);
                    }
                    auto nativeMethod = GenerateNativeMethod(*funcDecl.get(), enumDecl, genericConfig);
                    if (nativeMethod != nullptr) {
                        nativeMethod->identifier = getSignature;
                        generatedDecls.push_back(std::move(nativeMethod));
                    }
                }
            } else {
                auto getSignature = GetJniMethodNameForProp(propDecl, false);
                auto nativeMethod = GenerateNativeMethod(*funcDecl.get(), enumDecl);
                if (nativeMethod != nullptr) {
                    nativeMethod->identifier = getSignature;
                    generatedDecls.push_back(std::move(nativeMethod));
                }
            }
        }
    }

    generatedDecls.push_back(GenerateCJMappingNativeDeleteCjObjectFunc(enumDecl));
}

void JavaDesugarManager::GenerateForCJExtendMapping(AST::ExtendDecl& extendDecl)
{
    CJC_ASSERT(IsCJMapping(extendDecl));

    if (auto rt = DynamicCast<const RefType*>(extendDecl.extendedType.get())) {
        if (IsImpl(*rt->ref.target)) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_extend_ref_target_cannot_be_java_impl, extendDecl);
            return;
        }
    }

    for (auto& member : extendDecl.GetMemberDecls()) {
        if (member->TestAttr(Attribute::IS_BROKEN) || !member->TestAttr(Attribute::PUBLIC)) {
            continue;
        }
        if (auto fd = As<ASTKind::FUNC_DECL>(member.get())) {
            generatedDecls.push_back(GenerateNativeMethod(*fd, extendDecl));
        } else if (member->astKind == ASTKind::PROP_DECL && !member->TestAttr(Attribute::COMPILER_ADD)) {
            const PropDecl& propDecl = *StaticAs<ASTKind::PROP_DECL>(member.get());
            if (!propDecl.getters.empty()) {
                const OwnedPtr<FuncDecl>& getFuncDecl = propDecl.getters[0];
                auto getSignature = GetJniMethodNameForProp(propDecl, false);
                auto nativeGetMethod = GenerateNativeMethod(*getFuncDecl.get(), extendDecl);
                if (nativeGetMethod != nullptr) {
                    nativeGetMethod->identifier = getSignature;
                    generatedDecls.push_back(std::move(nativeGetMethod));
                }
            }
            if (!propDecl.setters.empty()) {
                const OwnedPtr<FuncDecl>& setFuncDecl = propDecl.setters[0];
                auto setSignature = GetJniMethodNameForProp(propDecl, true);
                auto nativeSetMethod = GenerateNativeMethod(*setFuncDecl.get(), extendDecl);
                if (nativeSetMethod != nullptr) {
                    nativeSetMethod->identifier = setSignature;
                    generatedDecls.push_back(std::move(nativeSetMethod));
                }
            }
        }
    }
}

OwnedPtr<CallExpr> JavaDesugarManager::CreateGetCJLambdaCallExpr(
    OwnedPtr<RefExpr> callResRef, Ptr<Ty> ty, const Decl& outerDecl)
{
    auto getCjLambdaFd = CheckCjLambdaDeclByTy(ty);
    auto wrapJavaEntity = lib.UnwrapJavaEntity(std::move(callResRef), lib.GetJobjectTy(), outerDecl, true);
    CJC_NULLPTR_CHECK(wrapJavaEntity);
    auto callExpr = CreateCall(getCjLambdaFd, outerDecl.curFile, std::move(wrapJavaEntity));
    return callExpr;
}

OwnedPtr<FuncDecl> JavaDesugarManager::GenerateInterfaceFwdclassMethod(
    AST::ClassDecl& fwdclassDecl, FuncDecl& interfaceFuncDecl, GenericConfigInfo* genericConfig)
{
    auto funcDecl = ASTCloner::Clone(Ptr<FuncDecl>(&interfaceFuncDecl));
    funcDecl->DisableAttr(Attribute::ABSTRACT, Attribute::OPEN);
    funcDecl->EnableAttr(Attribute::PUBLIC, Attribute::CJ_MIRROR_JAVA_INTERFACE_FWD);

    if (genericConfig) {
        ReplaceGenericTyForFunc(funcDecl, genericConfig, typeManager);
    }
    DesugarJavaMirrorMethod(*funcDecl, fwdclassDecl, genericConfig);
    funcDecl->outerDecl = Ptr<Decl>(&fwdclassDecl);

    return funcDecl;
}

OwnedPtr<AST::MemberAccess> JavaDesugarManager::GenThisMemAcessForSelfMethod(
    Ptr<FuncDecl> fd, Ptr<InterfaceDecl> interfaceDecl, GenericConfigInfo* genericConfig)
{
    Ptr<Ty> interfaceTy = TypeManager::GetInvalidTy();
    Ptr<Ty> funcTy = TypeManager::GetInvalidTy();
    if (genericConfig) {
        // init interfaceTy
        std::vector<Ptr<Ty>> typeArgs;
        for (const auto& typePair : genericConfig->instTypes) {
            std::string typeStr = typePair.second;
            auto ty = GetTyByName(typeStr);
            typeArgs.push_back(ty);
        }
        interfaceTy = typeManager.GetInterfaceTy(*interfaceDecl, typeArgs);

        // init funtTy for generic method.
        std::vector<Ptr<Ty>> tmpParamTys;
        for (auto& param : fd->funcBody->paramLists[0]->params) {
            tmpParamTys.push_back(
                param->ty->HasGeneric() ? GetGenericInstTy(genericConfig, param->ty, typeManager) : param->ty);
        }
        Ptr<Ty> retTy = fd->funcBody->retType->ty->HasGeneric()
            ? GetGenericInstTy(genericConfig, fd->funcBody->retType->ty, typeManager)
            : fd->funcBody->retType->ty;
        std::vector<Ptr<Ty>> tmpTypeArgs;
        for (auto& typeArg : fd->ty->typeArgs) {
            tmpTypeArgs.push_back(
                typeArg->HasGeneric() ? GetGenericInstTy(genericConfig, typeArg, typeManager) : typeArg);
        }

        funcTy = typeManager.GetFunctionTy(tmpParamTys, retTy);
        funcTy->typeArgs = tmpTypeArgs;
    } else {
        interfaceTy = interfaceDecl->ty;
        funcTy = fd->ty;
    }
    auto thisRef = CreateThisRef(interfaceDecl, interfaceTy, interfaceDecl->curFile);
    auto ma = CreateMemberAccess(std::move(thisRef), *fd);
    ma->ty = funcTy;
    ma->isExposedAccess = false;
    return ma;
}

OwnedPtr<FuncDecl> JavaDesugarManager::GenerateInterfaceFwdclassDefaultMethod(
    AST::ClassDecl& fwdclassDecl, FuncDecl& interfaceFuncDecl, GenericConfigInfo* genericConfig)
{
    auto replaceRefCall = [this, &interfaceFuncDecl, genericConfig](const Node&, Node& target) {
        auto targetPtr = Ptr<Node>(&target);
        if (genericConfig && targetPtr->ty->HasGeneric()) {
            targetPtr->ty = GetGenericInstTy(genericConfig, targetPtr->ty, typeManager);
        }
        if (Ptr<CallExpr> call = As<ASTKind::CALL_EXPR>(targetPtr.get())) {
            if (genericConfig && call->ty->HasGeneric()) {
                call->ty = GetGenericInstTy(genericConfig, call->ty, typeManager);
            }
            if (Ptr<RefExpr> refE = As<ASTKind::REF_EXPR>(call->baseFunc.get())) {
                if (Ptr<FuncDecl> fd = As<ASTKind::FUNC_DECL>(refE->GetTarget())) {
                    if (fd->outerDecl && fd->outerDecl == interfaceFuncDecl.outerDecl) {
                        auto interfaceDecl = As<ASTKind::INTERFACE_DECL>(interfaceFuncDecl.outerDecl);
                        auto ma = GenThisMemAcessForSelfMethod(fd, interfaceDecl, genericConfig);
                        ma->callOrPattern = call.get();
                        call->baseFunc = std::move(ma);
                    }
                }
            }
        }
    };
    OwnedPtr<FuncDecl> funcStub = ASTCloner::Clone(Ptr(&interfaceFuncDecl), replaceRefCall);
    funcStub->DisableAttr(Attribute::DEFAULT);
    funcStub->EnableAttr(Attribute::CJ_MIRROR_JAVA_INTERFACE_DEFAULT, AST::Attribute::COMPILER_ADD);

    if (genericConfig) {
        ReplaceGenericTyForFunc(funcStub, genericConfig, typeManager);
        funcStub->funcBody->parentClassLike = &fwdclassDecl;
    }

    // remove foreign anno from cloned func decl
    for (auto it = funcStub->annotations.begin(); it != funcStub->annotations.end(); ++it) {
        if ((*it)->kind == AnnotationKind::FOREIGN_NAME) {
            funcStub->annotations.erase(it);
            break;
        }
    }

    funcStub->outerDecl = Ptr(&fwdclassDecl);
    funcStub->identifier = funcStub->identifier.Val() + JAVA_INTERFACE_FWD_CLASS_DEFAULT_METHOD_SUFFIX;
    return funcStub;
}

void JavaDesugarManager::GenerateInterfaceFwdclassBody(
    AST::ClassDecl& fwdclassDecl, AST::InterfaceDecl& interfaceDecl, GenericConfigInfo* genericConfig)
{
    InsertJavaRefVarDecl(fwdclassDecl);
    InsertJavaMirrorCtor(fwdclassDecl, true);
    for (auto& decl : interfaceDecl.GetMemberDecls()) {
        if (FuncDecl* fd = As<ASTKind::FUNC_DECL>(decl.get());
            fd && !fd->TestAttr(Attribute::CONSTRUCTOR) && !fd->TestAttr(Attribute::STATIC)) {
            fwdclassDecl.body->decls.push_back(GenerateInterfaceFwdclassMethod(fwdclassDecl, *fd, genericConfig));
            if (fd->TestAttr(Attribute::DEFAULT)) {
                fwdclassDecl.body->decls.push_back(
                    GenerateInterfaceFwdclassDefaultMethod(fwdclassDecl, *fd, genericConfig));
            }
        } else if (auto prop = As<ASTKind::PROP_DECL>(decl.get())) {
            // not support yet
            auto message = "property in interface";
            diag.DiagnoseRefactor(DiagKindRefactor::sema_java_interop_not_supported, *prop, message);
        }
    }
}

OwnedPtr<ClassDecl> JavaDesugarManager::InitInterfaceFwdClassDecl(AST::InterfaceDecl& interfaceDecl)
{
    auto fwdclassDecl = MakeOwned<ClassDecl>();
    fwdclassDecl->identifier = interfaceDecl.identifier.Val() + JAVA_FWD_CLASS_SUFFIX;
    fwdclassDecl->identifier.SetPos(interfaceDecl.identifier.Begin(), interfaceDecl.identifier.End());
    fwdclassDecl->fullPackageName = interfaceDecl.fullPackageName;
    fwdclassDecl->moduleName = ::Cangjie::Utils::GetRootPackageName(interfaceDecl.fullPackageName);
    fwdclassDecl->curFile = interfaceDecl.curFile;
    fwdclassDecl->EnableAttr(Attribute::PUBLIC, Attribute::COMPILER_ADD, Attribute::CJ_MIRROR_JAVA_INTERFACE_FWD);
    fwdclassDecl->body = MakeOwned<ClassBody>();
    return fwdclassDecl;
}

void JavaDesugarManager::GenerateForCJInterfaceMapping(File& file, AST::InterfaceDecl& interfaceDecl)
{
    if (IsCJMappingGeneric(interfaceDecl)) {
        std::vector<GenericConfigInfo*> genericConfigsVector;
        bool isGenericGlueCode = false;
        InitGenericConfigs(file, &interfaceDecl, genericConfigsVector, isGenericGlueCode);
        for (const auto& config : genericConfigsVector) {
            auto fwdclassDecl = InitInterfaceFwdClassDecl(interfaceDecl);
            fwdclassDecl->identifier = config->declInstName + JAVA_FWD_CLASS_SUFFIX;

            // Set fwdclassDecl inheritedTypes.
            auto interfaceRefType = CreateRefType(interfaceDecl);
            std::vector<Ptr<Ty>> typeArgs;
            std::vector<OwnedPtr<Type>> typeArguments;
            for (const auto& typePair : config->instTypes) {
                std::string typeStr = typePair.second;
                auto ty = GetTyByName(typeStr);
                typeArgs.push_back(ty);
                auto priType = GetTypeByName(typeStr);
                interfaceRefType->typeArguments.emplace_back(std::move(priType));
            }
            interfaceRefType->ty = typeManager.GetInterfaceTy(interfaceDecl, typeArgs);
            fwdclassDecl->inheritedTypes.emplace_back(std::move(interfaceRefType));
            fwdclassDecl->ty = typeManager.GetClassTy(*fwdclassDecl, {});

            auto classLikeTy = DynamicCast<ClassLikeTy*>(interfaceDecl.ty);
            CJC_ASSERT(classLikeTy);
            classLikeTy->directSubtypes.insert(fwdclassDecl->ty);

            GenerateInterfaceFwdclassBody(*fwdclassDecl, interfaceDecl, config);
            generatedDecls.push_back(std::move(fwdclassDecl));
        }
    } else {
        auto fwdclassDecl = InitInterfaceFwdClassDecl(interfaceDecl);
        fwdclassDecl->inheritedTypes.emplace_back(CreateRefType(interfaceDecl));
        fwdclassDecl->ty = typeManager.GetClassTy(*fwdclassDecl, interfaceDecl.ty->typeArgs);
        auto classLikeTy = DynamicCast<ClassLikeTy*>(interfaceDecl.ty);
        CJC_ASSERT(classLikeTy);
        classLikeTy->directSubtypes.insert(fwdclassDecl->ty);
        GenerateInterfaceFwdclassBody(*fwdclassDecl, interfaceDecl);
        generatedDecls.push_back(std::move(fwdclassDecl));
    }
}

void JavaDesugarManager::InsertJavaObjectControllerVarDecl(ClassDecl& fwdClassDecl, ClassDecl& classDecl)
{
    auto& javaObjectControllerDecl = *lib.GetJavaObjectControllerDecl();
    auto controllerRefType = CreateRefType(javaObjectControllerDecl);

    auto instantTy = typeManager.GetClassTy(classDecl, classDecl.ty->typeArgs);
    auto varTy = typeManager.GetClassTy(javaObjectControllerDecl, {std::move(instantTy)});
    controllerRefType->ty = varTy;

    auto instantiationRefType = CreateRefType(classDecl);
    controllerRefType->typeArguments.emplace_back(std::move(instantiationRefType));

    auto javaObjectControllerVarDecl = CreateVarDecl(JAVA_OBJECT_CONTROLLER_NAME, nullptr, controllerRefType);
    javaObjectControllerVarDecl->ty = varTy;
    javaObjectControllerVarDecl->curFile = fwdClassDecl.curFile;

    Modifier publicMod = Modifier(TokenKind::PUBLIC, javaObjectControllerVarDecl->begin);
    publicMod.curFile = fwdClassDecl.curFile;
    javaObjectControllerVarDecl->modifiers.emplace(std::move(publicMod));

    javaObjectControllerVarDecl->isVar = false;
    javaObjectControllerVarDecl->outerDecl = Ptr(&fwdClassDecl);
    javaObjectControllerVarDecl->fullPackageName = fwdClassDecl.fullPackageName;

    fwdClassDecl.body->decls.push_back(std::move(javaObjectControllerVarDecl));
}

void JavaDesugarManager::InsertOverrideMaskVar(AST::ClassDecl& fwdClassDecl)
{
    auto overrideMaskVar =
        CreateVarDecl(JAVA_OVERRIDE_MASK_NAME, nullptr, GetPrimitiveType("UInt64", AST::TypeKind::TYPE_UINT64));
    overrideMaskVar->ty = TypeManager::GetPrimitiveTy(AST::TypeKind::TYPE_UINT64);
    overrideMaskVar->curFile = fwdClassDecl.curFile;

    Modifier publicMod = Modifier(TokenKind::PUBLIC, overrideMaskVar->begin);
    publicMod.curFile = fwdClassDecl.curFile;
    overrideMaskVar->modifiers.emplace(std::move(publicMod));

    overrideMaskVar->isVar = false;
    overrideMaskVar->outerDecl = Ptr(&fwdClassDecl);
    overrideMaskVar->fullPackageName = fwdClassDecl.fullPackageName;

    fwdClassDecl.body->decls.push_back(std::move(overrideMaskVar));
}

OwnedPtr<FuncDecl> JavaDesugarManager::GenerateFwdClassCtor(
    ClassDecl& fwdDecl, ClassDecl& classDecl, FuncDecl& oriCtorDecl)
{
    auto ctor = ASTCloner::Clone(Ptr(&oriCtorDecl));

    auto curFile = classDecl.curFile;

    auto& javaEntityDecl = *lib.GetJavaEntityDecl();
    auto int64Type = GetPrimitiveType("UInt64", AST::TypeKind::TYPE_UINT64);
    auto javaEntityFuncParam = CreateFuncParam("$ref", CreateRefType(javaEntityDecl), nullptr, javaEntityDecl.ty);
    auto maskFuncParam =
        CreateFuncParam("mask", std::move(int64Type), nullptr, TypeManager::GetPrimitiveTy(AST::TypeKind::TYPE_UINT64));

    std::vector<Ptr<Ty>> paramTys = {javaEntityFuncParam->ty, maskFuncParam->ty};
    for (auto paramTy : StaticCast<FuncTy*>(oriCtorDecl.ty.get())->paramTys) {
        paramTys.push_back(paramTy);
    }

    // func call name
    // param
    auto initDecl = lib.GetJavaObjectControllerInitDecl();
    auto javaEntityParamExpr = CreateRefExpr(*javaEntityFuncParam);
    auto strTy = initDecl->funcBody->paramLists[0]->params[1]->ty;
    auto classNameExpr =
        CreateLitConstExpr(LitConstKind::STRING, utils.GetJavaClassNormalizeSignature(*classDecl.ty), strTy);

    auto lhsController = WithinFile(CreateRefExpr(*GetFwdClassField(fwdDecl, JAVA_OBJECT_CONTROLLER_NAME)), curFile);
    auto rhsController =
        lib.CreateJavaObjectControllerCall(std::move(javaEntityParamExpr), std::move(classNameExpr), classDecl);
    auto controllerAssignment = CreateAssignExpr(
        std::move(lhsController), std::move(rhsController), TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));

    auto lhsMask = WithinFile(CreateRefExpr(*GetFwdClassField(fwdDecl, JAVA_OVERRIDE_MASK_NAME)), curFile);
    auto rhsMask = WithinFile(CreateRefExpr(*maskFuncParam), curFile);
    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto maskAssignment = CreateAssignExpr(std::move(lhsMask), std::move(rhsMask), unitTy);

    auto ctorTy = typeManager.GetFunctionTy(paramTys, fwdDecl.ty);

    auto superCall = CreateSuperCall(*oriCtorDecl.outerDecl, oriCtorDecl, oriCtorDecl.ty);
    for (auto& param : ctor->funcBody->paramLists[0]->params) {
        auto paramRef = WithinFile(CreateRefExpr(*param), curFile);
        superCall->args.push_back(CreateFuncArg(std::move(paramRef)));
    }

    ctor->funcBody->paramLists[0]->params.insert(
        ctor->funcBody->paramLists[0]->params.begin(), std::move(maskFuncParam));
    ctor->funcBody->paramLists[0]->params.insert(
        ctor->funcBody->paramLists[0]->params.begin(), std::move(javaEntityFuncParam));

    auto& block = ctor->funcBody->body;
    block->body.clear();

    block->body.emplace_back(std::move(superCall));
    block->body.emplace_back(std::move(controllerAssignment));
    block->body.emplace_back(std::move(maskAssignment));

    ctor->funcBody->ty = ctorTy;
    ctor->ty = ctorTy;
    ctor->outerDecl = &fwdDecl;
    ctor->funcBody->funcDecl = ctor.get();
    ctor->funcBody->parentClassLike = &classDecl;
    return ctor;
}

void JavaDesugarManager::InsertAttachCJObject(ClassDecl& fwdDecl, ClassDecl& classDecl)
{
    auto curFile = fwdDecl.curFile;

    auto attachCJObjectDecl = lib.GetAttachCJObjectDecl();
    auto javaCffiEntityDecl = lib.GetJavaEntityDecl();

    auto javaCffiEntityTy = lib.GetJavaEntityTy();

    auto javaEnvFuncParam = lib.CreateEnvFuncParam();

    auto funcTy = typeManager.GetFunctionTy({javaEnvFuncParam->ty}, javaCffiEntityTy);

    std::vector<OwnedPtr<Node>> bodyNodes;

    auto controllerRefExpr =
        WithinFile(CreateRefExpr(*GetFwdClassField(fwdDecl, JAVA_OBJECT_CONTROLLER_NAME)), curFile);
    auto funcAccess = CreateMemberAccess(std::move(controllerRefExpr), *attachCJObjectDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    auto javaEnvRefExpr = CreateRefExpr(*javaEnvFuncParam);
    args.push_back(CreateFuncArg(std::move(javaEnvRefExpr)));

    auto methodCall = CreateCallExpr(
        std::move(funcAccess), std::move(args), attachCJObjectDecl, javaCffiEntityTy, CallKind::CALL_DECLARED_FUNCTION);
    auto returnExpr = CreateReturnExpr(std::move(methodCall));
    bodyNodes.push_back(std::move(returnExpr));

    std::vector<OwnedPtr<FuncParam>> params;
    params.push_back(std::move(javaEnvFuncParam));
    auto paramList = CreateFuncParamList(std::move(params));
    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(std::move(paramList));

    auto funcBody = CreateFuncBody(std::move(paramLists), CreateRefType(*javaCffiEntityDecl),
        CreateBlock(std::move(bodyNodes), javaCffiEntityTy), javaCffiEntityTy);

    auto fd = CreateFuncDecl(attachCJObjectDecl->identifier.Val(), std::move(funcBody), funcTy);

    fd->fullPackageName = fwdDecl.fullPackageName;
    fd->outerDecl = &fwdDecl;
    fd->funcBody->funcDecl = fd.get();
    fd->constructorCall = ConstructorCall::SUPER;
    fd->EnableAttr(Attribute::PUBLIC, Attribute::IN_CLASSLIKE);
    fd->funcBody->parentClassLike = &classDecl;

    fwdDecl.body->decls.emplace_back(std::move(fd));
}

OwnedPtr<FuncDecl> JavaDesugarManager::GenerateFwdClassMethod(
    ClassDecl& fwdDecl, ClassDecl& classDecl, FuncDecl& oriMethodDecl, int index)
{
    auto fun = ASTCloner::Clone(Ptr(&oriMethodDecl));
    auto curFile = classDecl.curFile;
    CJC_NULLPTR_CHECK(curFile);

    // generate thenbody
    // let env = Java_CFFI_get_env()
    OwnedPtr<CallExpr> jniEnvCall = lib.CreateGetJniEnvCall(curFile);
    if (!jniEnvCall) {
        fun->EnableAttr(Attribute::IS_BROKEN);
        return nullptr;
    }
    auto envVar = CreateVarDecl(ENV, std::move(jniEnvCall), nullptr);

    // let localRef = attachCJObject(env)
    auto attachCJObjectDecl = GetFwdClassMethod(fwdDecl, "attachCJObject");
    auto attachRefExpr = WithinFile(CreateRefExpr(*envVar), curFile);
    auto attachCallExpr = CreateCall(attachCJObjectDecl, curFile, std::move(attachRefExpr));
    auto localRefVar = CreateVarDecl("localRef", std::move(attachCallExpr), nullptr);

    // deleteLocalRef(env, localRef)
    auto deleteLocalRefDecl = lib.GetDeleteLocalRefDecl();
    std::vector<OwnedPtr<FuncArg>> deleteCallArgs;
    auto deleteEnvRef = WithinFile(CreateRefExpr(*envVar), curFile);
    auto deleteLocalRef = WithinFile(CreateRefExpr(*localRefVar), curFile);
    auto deleteCallExpr = CreateCall(deleteLocalRefDecl, curFile, std::move(deleteEnvRef), std::move(deleteLocalRef));

    // Java_CFFI_callVirtualMethod
    auto& paramList = *fun->funcBody->paramLists[0].get();
    auto callEnvRef = WithinFile(CreateRefExpr(*envVar), curFile);
    auto callLocalRef = WithinFile(CreateRefExpr(*localRefVar), curFile);
    OwnedPtr<Expr> methodCall = lib.CreateCFFICallMethodCall(
        std::move(callEnvRef), std::move(callLocalRef), MemberJNISignature(utils, *fun), paramList, *curFile);
    if (!methodCall) {
        fun->EnableAttr(Attribute::IS_BROKEN);
        return nullptr;
    }

    auto methodCallRes = WithinFile(CreateTmpVarDecl(nullptr, std::move(methodCall)), curFile);
    auto callResRef = WithinFile(CreateRefExpr(*methodCallRes), curFile);

    OwnedPtr<Expr> retCallExpr = nullptr;
    if (fun->funcBody->retType->ty->kind == TypeKind::TYPE_FUNC) {
        retCallExpr = CreateGetCJLambdaCallExpr(std::move(callResRef), fun->funcBody->retType->ty, fwdDecl);
    } else {
        retCallExpr = lib.UnwrapJavaEntity(std::move(callResRef), fun->funcBody->retType->ty, fwdDecl);
    }

    if (!retCallExpr) {
        fun->EnableAttr(Attribute::IS_BROKEN);
        return nullptr;
    }

    std::vector<OwnedPtr<Node>> thenBodyNodes;
    auto thenRetExpr = CreateReturnExpr(std::move(retCallExpr), fun->funcBody.get());
    thenRetExpr->ty = TypeManager::GetNothingTy();
    thenRetExpr->refFuncBody = fun->funcBody.get();
    thenBodyNodes.push_back(std::move(envVar));
    thenBodyNodes.push_back(std::move(localRefVar));
    thenBodyNodes.push_back(std::move(methodCallRes));
    thenBodyNodes.push_back(std::move(deleteCallExpr));
    thenBodyNodes.push_back(std::move(thenRetExpr));
    auto thenBodyBlock = CreateBlock(std::move(thenBodyNodes), fun->funcBody->retType->ty);

    // generate elsebody
    auto superRef = WithinFile(CreateSuperRef(Ptr(&classDecl), classDecl.ty), curFile);
    auto superMemAcess = CreateMemberAccess(std::move(superRef), oriMethodDecl);
    std::vector<OwnedPtr<FuncArg>> superCallArgs;
    for (auto& param : fun->funcBody->paramLists[0]->params) {
        auto paramRef = WithinFile(CreateRefExpr(*param), curFile);
        superCallArgs.push_back(CreateFuncArg(std::move(paramRef)));
    }
    auto superCall = CreateCallExpr(std::move(superMemAcess), std::move(superCallArgs), Ptr(&oriMethodDecl),
        oriMethodDecl.ty, CallKind::CALL_DECLARED_FUNCTION);
    auto elseRetExpr = CreateReturnExpr(std::move(superCall), fun->funcBody.get());
    std::vector<OwnedPtr<Node>> elseBodyNodes;
    elseBodyNodes.push_back(std::move(elseRetExpr));
    auto elseBodyBlock = CreateBlock(std::move(elseBodyNodes), fun->funcBody->retType->ty);

    // generate condexpr (overrideMask & 1) != 0
    auto uint64Ty = typeManager.GetPrimitiveTy(TypeKind::TYPE_UINT64);
    auto litExprZero = CreateLitConstExpr(LitConstKind::INTEGER, "0", uint64Ty);
    unsigned long mask = 1 << index;
    auto litExprOne = CreateLitConstExpr(LitConstKind::INTEGER, std::to_string(mask), uint64Ty);
    auto overrideMaskRefExpr = WithinFile(CreateRefExpr(*GetFwdClassField(fwdDecl, JAVA_OVERRIDE_MASK_NAME)), curFile);
    auto binaryExpr1 = CreateBinaryExpr(std::move(overrideMaskRefExpr), std::move(litExprOne), TokenKind::BITAND);
    auto parenExpr = MakeOwned<ParenExpr>();
    parenExpr->expr = std::move(binaryExpr1);
    parenExpr->EnableAttr(Attribute::COMPILER_ADD);
    parenExpr->ty = uint64Ty;
    auto condExpr = CreateBinaryExpr(std::move(parenExpr), std::move(litExprZero), TokenKind::NOTEQ);
    condExpr->ty = typeManager.GetPrimitiveTy(TypeKind::TYPE_BOOLEAN);

    auto ifExpr = CreateIfExpr(
        std::move(condExpr), std::move(thenBodyBlock), std::move(elseBodyBlock), fun->funcBody->retType->ty);
    std::vector<OwnedPtr<Node>> funcBodyNodes;
    funcBodyNodes.push_back(std::move(ifExpr));

    fun->funcBody->body = CreateBlock(std::move(funcBodyNodes), fun->funcBody->retType->ty);
    fun->funcBody->ty = TypeManager::GetNothingTy();

    fun->outerDecl = &fwdDecl;
    fun->funcBody->parentClassLike = &classDecl;

    return fun;
}

void JavaDesugarManager::GenerateClassFwdclassBody(AST::ClassDecl& fwdClassDecl, AST::ClassDecl& classDecl,
    std::vector<std::pair<Ptr<FuncDecl>, Ptr<FuncDecl>>>& pairCtors)
{
    InsertJavaObjectControllerVarDecl(fwdClassDecl, classDecl);
    InsertOverrideMaskVar(fwdClassDecl);
    for (auto& decl : classDecl.GetMemberDecls()) {
        if (decl->TestAnyAttr(Attribute::IS_BROKEN, Attribute::PRIVATE, Attribute::INTERNAL, Attribute::DEFAULT)) {
            continue;
        }
        auto fd = As<ASTKind::FUNC_DECL>(decl.get());
        if (fd && fd->TestAttr(Attribute::CONSTRUCTOR) && !fd->TestAttr(Attribute::STATIC)) {
            auto fwdCtor = GenerateFwdClassCtor(fwdClassDecl, classDecl, *fd);
            pairCtors.emplace_back(fd, fwdCtor);
            fwdClassDecl.body->decls.push_back(std::move(fwdCtor));
        }
    }

    InsertAttachCJObject(fwdClassDecl, classDecl);

    int index = 0;
    for (auto& decl : classDecl.GetMemberDecls()) {
        if (decl->TestAnyAttr(Attribute::IS_BROKEN, Attribute::PRIVATE, Attribute::INTERNAL, Attribute::DEFAULT)) {
            continue;
        }
        if (auto fd = As<ASTKind::FUNC_DECL>(decl.get()); fd && !fd->TestAttr(Attribute::CONSTRUCTOR) &&
            !fd->TestAttr(Attribute::STATIC) && fd->TestAttr(Attribute::OPEN)) {
            fwdClassDecl.body->decls.push_back(GenerateFwdClassMethod(fwdClassDecl, classDecl, *fd, index));
            index++;
        }
    }
}

void JavaDesugarManager::GenerateForCJOpenClassMapping(AST::ClassDecl& classDecl)
{
    auto fwdclassDecl = MakeOwned<ClassDecl>();
    fwdclassDecl->identifier = classDecl.identifier.Val() + JAVA_FWD_CLASS_SUFFIX;
    fwdclassDecl->identifier.SetPos(classDecl.identifier.Begin(), classDecl.identifier.End());
    fwdclassDecl->fullPackageName = classDecl.fullPackageName;
    fwdclassDecl->moduleName = ::Cangjie::Utils::GetRootPackageName(classDecl.fullPackageName);
    fwdclassDecl->curFile = classDecl.curFile;

    fwdclassDecl->inheritedTypes.emplace_back(CreateRefType(classDecl));

    fwdclassDecl->ty = typeManager.GetClassTy(*fwdclassDecl, classDecl.ty->typeArgs);
    auto classLikeTy = DynamicCast<ClassLikeTy*>(classDecl.ty);
    CJC_ASSERT(classLikeTy);
    classLikeTy->directSubtypes.insert(fwdclassDecl->ty);

    fwdclassDecl->EnableAttr(Attribute::PUBLIC, Attribute::COMPILER_ADD, Attribute::CJ_MIRROR_JAVA_INTERFACE_FWD);

    fwdclassDecl->body = MakeOwned<ClassBody>();

    std::vector<std::pair<Ptr<FuncDecl>, Ptr<FuncDecl>>> pairCtors;
    GenerateClassFwdclassBody(*fwdclassDecl, classDecl, pairCtors);

    std::vector<FuncDecl*> generatedCtors;
    for (auto& member : classDecl.GetMemberDecls()) {
        if (member->TestAnyAttr(Attribute::IS_BROKEN, Attribute::PRIVATE)) {
            continue;
        }
        auto fd = As<ASTKind::FUNC_DECL>(member.get());
        if (fd && !fd->TestAttr(Attribute::CONSTRUCTOR)) {
            auto nativeMethod = GenerateNativeMethod(*fd, classDecl);
            if (nativeMethod != nullptr) {
                generatedDecls.push_back(std::move(nativeMethod));
            }
            continue;
        }
    }

    if (!pairCtors.empty()) {
        generatedDecls.push_back(GenerateCJMappingNativeDetachCjObjectFunc(*fwdclassDecl, classDecl));
        for (const auto& pair : pairCtors) {
            generatedDecls.push_back(GenerateNativeInitCjObjectFunc(*pair.first, false, true, pair.second));
        }
    }

    generatedDecls.push_back(std::move(fwdclassDecl));
}

void JavaDesugarManager::GenerateNativeForCJInterfaceMapping(AST::ClassDecl& classDecl)
{
    CJC_ASSERT(IsFwdClass(classDecl));

    for (auto& member : classDecl.GetMemberDecls()) {
        if (member->TestAttr(Attribute::IS_BROKEN) || !member->TestAttr(Attribute::PUBLIC)) {
            continue;
        }
        if (!member->TestAttr(Attribute::CJ_MIRROR_JAVA_INTERFACE_DEFAULT)) {
            continue;
        }
        if (auto fd = As<ASTKind::FUNC_DECL>(member.get())) {
            generatedDecls.push_back(GenerateNativeMethod(*fd, classDecl));
        }
    }
}

void JavaDesugarManager::PreGenerateInCJMapping(File& file)
{
    if (!isInitLambdaUtilFunc) {
        GenerateLambdaGlueCode(file);
    }
}

OwnedPtr<Decl> JavaDesugarManager::GenerateCallImplNativeMethod(File& file, LambdaPattern& lambdaPattern)
{
    Ptr<Decl> decl = GetLambdaTmpDecl(file, GetLambdaJavaClassName(lambdaPattern), file.curPackage->fullPackageName);
    std::vector<OwnedPtr<FuncParam>> params;
    FuncParam* jniEnvPtrParam = nullptr;
    OwnedPtr<Expr> selfParamRef;
    GenerateFuncParamsForNativeDeleteCjObject(*decl, params, jniEnvPtrParam, selfParamRef);
    OwnedPtr<Expr> envParamRef = WithinFile(CreateRefExpr(*jniEnvPtrParam), decl->curFile);

    auto regCall =
        lib.CreateGetFromRegistryCall(std::move(envParamRef), std::move(selfParamRef), GetLambdaFuncTy(lambdaPattern));

    auto retTy = GetTyByName(lambdaPattern.returnType);

    std::vector<OwnedPtr<FuncArg>> funcArgs;

    int index = 1;
    for (auto& typeName : lambdaPattern.parameterTypes) {
        // current just support primitive type, can expand.
        OwnedPtr<FuncParam> param =
            CreateFuncParam("p" + std::to_string(index), nullptr, nullptr, GetTyByName(typeName));
        auto paramRef = WithinFile(CreateRefExpr(*param), decl->curFile);
        funcArgs.push_back(CreateFuncArg(std::move(paramRef)));
        params.push_back(std::move(param));
        index++;
    }

    auto methodCall =
        CreateCallExpr(std::move(regCall), std::move(funcArgs), nullptr, retTy, CallKind::CALL_DECLARED_FUNCTION);
    auto methodCallRes = CreateTmpVarDecl(nullptr, std::move(methodCall));

    // ret param expr, current just support primitiveg type, can expand
    auto retExpr = WithinFile(CreateRefExpr(*methodCallRes), decl->curFile);

    auto wrappedNodesLambda = WrapReturningLambdaExpr(typeManager, Nodes(std::move(methodCallRes), std::move(retExpr)));

    auto funcName = GetLambdaCallImplJniMethodName(*decl);

    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(CreateFuncParamList(std::move(params)));

    return GenerateNativeFuncDeclBylambda(*decl, wrappedNodesLambda, paramLists, *jniEnvPtrParam, retTy, funcName);
}

Ptr<FuncTy> JavaDesugarManager::GetLambdaFuncTy(LambdaPattern& lambdaPattern)
{
    std::vector<Ptr<Ty>> lambdaParamTys;
    for (auto& paramStr : lambdaPattern.parameterTypes) {
        lambdaParamTys.push_back(GetTyByName(paramStr));
    }
    auto retTy = typeManager.GetFunctionTy(lambdaParamTys, GetTyByName(lambdaPattern.returnType));
    return retTy;
}

Ptr<Decl> JavaDesugarManager::GetLambdaTmpDecl(File& file, std::string javaClassName, std::string fullPackageName)
{
    Ptr<Decl> decl = new Decl();
    decl->curFile = file.curFile;
    if (file.decls.size() > 0) {
        decl->moduleName = file.decls[0]->moduleName;
    }
    decl->fullPackageName = fullPackageName;
    decl->identifier = javaClassName + "." + "00024Box";
    return decl;
}

std::string JavaDesugarManager::GetLambdaCallImplJniMethodName(Decl& decl)
{
    auto sampleJavaName = "callImpl";
    std::string fqname = GetJavaFQName(decl);
    MangleJNIName(fqname);

    return "Java_" + fqname + "_" + sampleJavaName;
}

void JavaDesugarManager::GenerateLambdaGlueCode(File& file)
{
    CJC_ASSERT(file.curPackage);
    auto curFile = file.curFile;
    auto fullPackageName = file.curPackage->fullPackageName;
    std::vector<LambdaPattern> lambdaPatterns = file.curPackage->lambdaPatterns;
    if (lambdaPatterns.size() <= 0) {
        return;
    }
    for (auto& lambdaPattern : lambdaPatterns) {
        lambdaPattern.fullPackageName = fullPackageName;
        // generate cj glue code.
        // generate getInt32ToInt32CJLambda(javaLamba:jobject) : (Int32) -> Int32 decl
        std::string className = GetLambdaJavaClassName(lambdaPattern);
        std::vector<OwnedPtr<FuncParam>> params;
        PushObjParams(params, "javaLambda");
        auto& objParam = *params[0];
        auto funcParamList = CreateFuncParamList(std::move(params));

        auto lambdaTy = GetLambdaFuncTy(lambdaPattern);
        auto lambdaType = CreateFuncType(lambdaTy);

        auto lambdaExpr = WithinFile(GenerateLambdaExpr(file, lambdaPattern, objParam), curFile);
        auto returnExpr = WithinFile(CreateReturnExpr(std::move(lambdaExpr)), curFile);
        std::vector<OwnedPtr<Node>> nodes;
        nodes.push_back(std::move(returnExpr));
        auto block = CreateBlock(std::move(nodes), TypeManager::GetNothingTy());

        auto funcTy = typeManager.GetFunctionTy({objParam.ty}, lambdaTy);
        std::vector<OwnedPtr<FuncParamList>> paramLists;
        paramLists.push_back(std::move(funcParamList));
        auto body =
            WithinFile(CreateFuncBody(std::move(paramLists), std::move(lambdaType), std::move(block), funcTy), curFile);

        auto funcDecl = WithinFile(CreateFuncDecl("get" + className + "CJLambda", std::move(body), funcTy), curFile);
        funcDecl->fullPackageName = fullPackageName;
        funcDecl->funcBody->funcDecl = funcDecl.get();
        funcDecl->EnableAttr(Attribute::PUBLIC, Attribute::COMPILER_ADD, Attribute::GLOBAL, Attribute::INITIALIZED);
        if (file.decls.size() > 0) {
            funcDecl->moduleName = file.decls[0]->moduleName;
        }
        if (funcDecl) {
            auto tmpFuncDecl = funcDecl.get();
            generatedDecls.push_back(std::move(funcDecl));
            lambdaConfUtilFuncs.emplace(lambdaTy->String(), tmpFuncDecl);
        }

        // generate delete cj object
        Ptr<Decl> decl = GetLambdaTmpDecl(file, className, fullPackageName);
        generatedDecls.push_back(GenerateCJMappingNativeDeleteCjObjectFunc(*decl));

        // generate callImp native method
        generatedDecls.push_back(GenerateCallImplNativeMethod(file, lambdaPattern));

        // generate java source code.
        const std::string fileJ = className + ".java";
        auto codegen = JavaSourceCodeGenerator(mangler, typeManager, javaCodeGenPath, fileJ,
            GetCangjieLibName(outputLibPath, fullPackageName), &lambdaPattern);
        codegen.Generate();
    }
    isInitLambdaUtilFunc = true;
}

OwnedPtr<LambdaExpr> JavaDesugarManager::GenerateLambdaExpr(File& file, LambdaPattern& pattern, FuncParam& funcParam)
{
    auto curFile = file.curFile;
    // lambda func input param
    std::vector<OwnedPtr<FuncParam>> funcParams;
    std::vector<Ptr<Ty>> paramTys;
    int index = 1;
    for (auto& typeName : pattern.parameterTypes) {
        funcParams.push_back(
            CreateFuncParam("p" + std::to_string(index), GetTypeByName(typeName), nullptr, GetTyByName(typeName)));
        paramTys.push_back(GetTyByName(typeName));
        index++;
    }
    auto funcParamList = CreateFuncParamList(std::move(funcParams));

    // lambda func output param
    auto retType = GetTypeByName(pattern.returnType);
    auto retTy = GetTyByName(pattern.returnType);

    // block
    // let tmp = Java_CFFI_callVirtualMethod()
    OwnedPtr<Expr> objParamRef = WithinFile(CreateRefExpr(funcParam), curFile);
    MemberJNISignature MemberJNISignature(
        NormalizeJavaSignature(file.curPackage->fullPackageName + "/" + GetLambdaJavaClassName(pattern)), "call",
        utils.GetJavaTypeSignature(*retTy, paramTys));

    OwnedPtr<Expr> methodCall = lib.CreateCFFICallMethodCall(lib.CreateGetJniEnvCall(curFile),
        lib.CreateJavaEntityJobjectCall(std::move(objParamRef)), MemberJNISignature, *funcParamList, *curFile);
    CJC_NULLPTR_CHECK(methodCall);
    auto methodCallRes = WithinFile(CreateTmpVarDecl(nullptr, std::move(methodCall)), curFile);

    // return Java_CFFI_unwrapJavaEntityAsValue()
    auto callResRef = WithinFile(CreateRefExpr(*methodCallRes), curFile);
    Decl emptyDecl;
    auto unwrapJavaEntityCall = lib.UnwrapJavaEntity(std::move(callResRef), retTy, emptyDecl);
    CJC_NULLPTR_CHECK(unwrapJavaEntityCall);
    auto returnExpr = CreateReturnExpr(std::move(unwrapJavaEntityCall));
    returnExpr->ty = TypeManager::GetNothingTy();

    std::vector<OwnedPtr<Node>> nodes;
    nodes.push_back(std::move(methodCallRes));
    nodes.push_back(std::move(returnExpr));

    auto block = CreateBlock(std::move(nodes), TypeManager::GetNothingTy());

    auto funcTy = typeManager.GetFunctionTy(paramTys, retTy);

    std::vector<OwnedPtr<FuncParamList>> paramLists;
    paramLists.push_back(std::move(funcParamList));
    auto lambdaBody = CreateFuncBody(std::move(paramLists), std::move(retType), std::move(block), funcTy);
    auto lambdaExpr = CreateLambdaExpr(std::move(lambdaBody));
    return lambdaExpr;
}

void JavaDesugarManager::GenerateFwdClassInCJMapping(File& file)
{
    for (auto& decl : file.decls) {
        if (!decl.get()->TestAttr(Attribute::PUBLIC) || decl.get()->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }
        auto interfaceDecl = As<ASTKind::INTERFACE_DECL>(decl.get());
        if (interfaceDecl && IsCJMapping(*interfaceDecl)) {
            GenerateForCJInterfaceMapping(file, *interfaceDecl);
            continue;
        }

        auto classDecl = As<ASTKind::CLASS_DECL>(decl.get());
        if (classDecl && IsCJMapping(*classDecl) && classDecl->TestAttr(Attribute::OPEN)) {
            GenerateForCJOpenClassMapping(*classDecl);
            continue;
        }
    }
}

void JavaDesugarManager::GenerateInCJMapping(File& file)
{
    for (auto& decl : file.decls) {
        if (!decl.get()->TestAttr(Attribute::PUBLIC) || decl.get()->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }
        auto astDecl = As<ASTKind::DECL>(decl.get());
        if (astDecl && astDecl->TestAttr(Attribute::IS_BROKEN)) {
            continue;
        }
        std::vector<GenericConfigInfo*> genericConfigsVector;
        bool isGenericGlueCode = false;
        // Initialize generic-related data type information.
        InitGenericConfigs(file, decl.get(), genericConfigsVector, isGenericGlueCode);
        auto structDecl = As<ASTKind::STRUCT_DECL>(decl.get());
        if (file.curPackage.get()->isInteropCJPackageConfig && structDecl &&
            !structDecl->symbol->isNeedExposedToInterop) {
            continue;
        }
        if (structDecl && IsCJMapping(*structDecl)) {
            GenerateForCJStructOrClassTypeMapping(file, structDecl);
            continue;
        }
        auto enumDecl = As<ASTKind::ENUM_DECL>(decl.get());
        if (enumDecl && IsCJMapping(*enumDecl)) {
            GenerateForCJEnumMapping(*enumDecl);
            continue;
        }
        auto classDecl = As<ASTKind::CLASS_DECL>(decl.get());
        if (classDecl && IsCJMapping(*classDecl) && !classDecl->TestAttr(Attribute::OPEN)) {
            GenerateForCJStructOrClassTypeMapping(file, classDecl);
            continue;
        }
        if (classDecl && IsFwdClass(*classDecl)) {
            GenerateNativeForCJInterfaceMapping(*classDecl);
            continue;
        }
        auto extendDecl = As<ASTKind::EXTEND_DECL>(decl.get());
        if (extendDecl && IsCJMapping(*extendDecl)) {
            GenerateForCJExtendMapping(*extendDecl);
        }
    }
}

void JavaDesugarManager::DesugarInCJMapping(File& file)
{
    // origin reference decl mapping to its all extendDecl
    std::map<Ptr<Decl>, std::vector<Ptr<ExtendDecl>>> ref2extend;
    // origin reference decl which need generate java glue code file
    std::vector<Ptr<Decl>> genDecls;
    for (auto& decl : file.decls) {
        if (!decl.get()->TestAttr(Attribute::PUBLIC) || decl.get()->TestAttr(Attribute::IS_BROKEN) ||
            !JavaSourceCodeGenerator::IsDeclAppropriateForGeneration(*decl.get()) || !IsCJMapping(*decl.get())) {
            continue;
        }
        if (auto extendDecl = As<ASTKind::EXTEND_DECL>(decl.get())) {
            if (auto rt = DynamicCast<const RefType*>(extendDecl->extendedType.get())) {
                ref2extend[rt->ref.target].emplace_back(extendDecl);
            }
        } else {
            genDecls.emplace_back(decl.get());
        }
    }
    for (auto decl : genDecls) {
        if (IsCJMappingGeneric(*decl)) {
            std::vector<GenericConfigInfo*> genericConfigsVector;
            bool isGenericGlueCode = false;
            InitGenericConfigs(file, decl, genericConfigsVector, isGenericGlueCode);
            for (auto& config : genericConfigsVector) {
                const std::string fileJ = config->declInstName + ".java";
                auto codegen = JavaSourceCodeGenerator(decl.get(), mangler, typeManager, javaCodeGenPath, fileJ,
                    GetCangjieLibName(outputLibPath, decl.get()->GetFullPackageName()), config,
                    file.curPackage.get()->isInteropCJPackageConfig);
                codegen.Generate();
            }
        } else {
            const std::string fileJ = decl.get()->identifier.Val() + ".java";
            auto codegen = JavaSourceCodeGenerator(decl.get(), mangler, typeManager, javaCodeGenPath, fileJ,
                GetCangjieLibName(outputLibPath, decl.get()->GetFullPackageName()), ref2extend[decl],
                file.curPackage.get()->isInteropCJPackageConfig);
            codegen.Generate();
        }
    }
}

} // namespace Cangjie::Interop::Java
