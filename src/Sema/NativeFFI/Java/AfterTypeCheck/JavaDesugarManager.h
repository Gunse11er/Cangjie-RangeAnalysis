// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares core support for java mirror and mirror subtype
 */
#ifndef CANGJIE_SEMA_NATIVE_FFI_JAVA_DESUGAR_MANAGER
#define CANGJIE_SEMA_NATIVE_FFI_JAVA_DESUGAR_MANAGER

#include "Context.h"
#include "InteropLibBridge.h"
#include "Utils.h"

#include "cangjie/AST/Node.h"
#include "cangjie/Mangle/BaseMangler.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"
#include "InheritanceChecker/MemberSignature.h"
#include "cangjie/Utils/SafePointer.h"
#include <unordered_map>

namespace Cangjie::Native::FFI::Java {
class GenerateInJavaImplRegistryCompanion;
class GenerateNativeBridgeForJavaImpl;
class GenerateJavaImplWrappingConstructorStub;
class GenerateInJavaImplReferenceWrapper;
class GenerateJavaImplRegistryCompanionReferenceField;
class RewriteJavaImplReferenceWrapperFields;
class DesugarTypeCheckingAndCasting;
class DesugarSuperConstructorCallInJavaImplReferenceWrapper;
class DesugarSuperMethodCallInJavaImplReferenceWrapper;
class DesugarJArray;
class DesugarJavaImplSuperConstructorCall;
}

namespace Cangjie::Interop::Java {
using namespace AST;
using namespace std;
using namespace Native::FFI::Java;

const std::string JAVA_ARRAY_GET_FOR_REF_TYPES = "$javaarrayget";
const std::string JAVA_ARRAY_SET_FOR_REF_TYPES = "$javaarrayset";
const std::string JAVA_IMPL_ENTITY_ARG_NAME_IN_GENERATED_CTOR = "$obj";

enum class DesugarCJImplStage : uint8_t {
    BEGIN,
    PRE_GENERATE,
    FWD_GENERATE,
    IMPL_GENERATE,
    IMPL_DESUGAR,
    END
};

class JavaDesugarManager {
// TODO: remove JavaDesugarManager completely and avoid friendship.
friend class Cangjie::Native::FFI::Java::GenerateInJavaImplRegistryCompanion;
friend class Cangjie::Native::FFI::Java::GenerateNativeBridgeForJavaImpl;
friend class Cangjie::Native::FFI::Java::GenerateJavaImplWrappingConstructorStub;
friend class Cangjie::Native::FFI::Java::GenerateInJavaImplReferenceWrapper;
friend class Cangjie::Native::FFI::Java::GenerateJavaImplRegistryCompanionReferenceField;
friend class Cangjie::Native::FFI::Java::RewriteJavaImplReferenceWrapperFields;
friend class Cangjie::Native::FFI::Java::DesugarTypeCheckingAndCasting;
friend class Cangjie::Native::FFI::Java::DesugarSuperConstructorCallInJavaImplReferenceWrapper;
friend class Cangjie::Native::FFI::Java::DesugarSuperMethodCallInJavaImplReferenceWrapper;
friend class Cangjie::Native::FFI::Java::DesugarJArray;
friend class Cangjie::Native::FFI::Java::DesugarJavaImplSuperConstructorCall;
public:
    JavaDesugarManager(ImportManager& importManager, TypeManager& typeManager, DiagnosticEngine& diag,
        const BaseMangler& mangler, const std::optional<std::string>& javaCodeGenPath, const std::string& outputLibPath,
        const std::unordered_map<Ptr<const InheritableDecl>, MemberMap>& memberMap, Package& pkg)
        : importManager(importManager),
          typeManager(typeManager),
          utils(importManager, typeManager, pkg),
          diag(diag),
          mangler(mangler),
          lib(importManager, typeManager, diag, utils),
          javaCodeGenPath(javaCodeGenPath),
          outputLibPath(outputLibPath),
          memberMap(memberMap)
    {
        lib.CheckInteropLibVersion();
    }

    /**
     * Constructors generation and javaref field insertion.
     * The first step: generate members in `JObject` and insert empty constructor in other mirrors. ([doStub] = `false`)
     * The second step: fill pregenerated bodies ([doStub] = `false`)
     */
    void GenerateInMirrors(File& file, bool doStub);

    void GenerateInMirror(ClassDecl& classDecl, bool doStub);

    void GenerateInSynthetic(ClassDecl& cd);

    /**
     * Desugar constructors, methods, etc
     */
    void DesugarMirrors(File& file);

    void GenerateJavaSourceCode(AfterTypeCheckContext& ctx);

    void DesugarJavaMirror(ClassDecl& mirror);
    void DesugarJavaMirror(InterfaceDecl& mirror);
    void ProcessJavaMirrorImplStage(AfterTypeCheckContext& ctx, std::function<void(AST::Node&)> desugarPropRef);
    void ProcessCJImplStage(DesugarCJImplStage stage, File& file);

    /**
     * Generates glue code for CJMapping tuples:
     *
     * (Int32, Int32) is configured
     * after:
     * @C
     * public func Java_TupleOfInt32Int32_initCJObject(env, obj, item0: Int32, item1: Int32) {
     *     return Java_CFFI_put_to_registry_1((item0, item1))
     * }
     *
     * @C
     * public func Java_TupleOfInt32Int32_item0(env, obj, self: Int64) {
     *     return Java_CFFI_getFromRegistry<(Int32, Int32)>(env, self)[0]
     * }
     * ...
     *
     * @C
     * public func Java_TupleOfInt32Int32_deleteCJObject(env, obj, self: Int64) {
     *      Java_CFFI_removeFromRegistry(self)
     * }
     */
    void GenerateTuplesGlueCode(Package& pkg);

    /**
     * Generate forward class for CJMapping data structure
     */
    void GenerateFwdClassInCJMapping(File& file);

    /**
     * Generate constructors and native init/deinit/method call functions (callable from java) for CJMapping
     * data structure
     */
    void GenerateInCJMapping(File& file);

    /**
     * Desugar in CJMapping data structure
     */
    void DesugarInCJMapping(File& file);

private:
    /**
     * Processes logically isolated interop stage.
     */
    template <typename S, class... StageArgs>
    std::enable_if_t<std::is_base_of_v<AfterTypeCheckStage, S>, void>
    Process(AfterTypeCheckContext& ctx, StageArgs&&... args) const
    {
        S stage(std::forward<StageArgs>(args)...);
        stage(ctx);
    }

    /**
     * Inserts javaref decl to the class decl:
     *
     * let javaref: Java_CFFI_JavaEntity
     */
    void InsertJavaRefVarDecl(ClassDecl& decl);

    void InsertArrayJavaEntityGet(ClassDecl& decl);
    void InsertArrayJavaEntitySet(ClassDecl& decl);

    /**
     * Generates and inserts constructor in java mirror:
     *
     * public init($ref: Java_CFFI_JavaEntity) {
     *     this.javaref = ref // for JObject
     *     // super($ref) // for other mirrors
     * }
     */
    void InsertJavaMirrorCtor(ClassDecl& decl, bool doStub);

    /**
     * var $hasInited: Bool = false
     */
    void InsertJavaMirrorHasInited(ClassDecl& mirror);

    /**
     * ~init() {
     *     Java_CFFI_deleteGlobalReference($jnienv, this.javaref)
     * }
     */
    void InsertJavaMirrorFinalizer(ClassDecl& mirror);

    /**
     * Generates and inserts javaref getter as javaref field will be in synthetic class
     * that implements current interface.
     *
     * abstract getter
     * public func $getJavaRef(): Java_CFFI_JavaEntity
     */
    void InsertAbstractJavaRefGetter(ClassLikeDecl& decl);

    /**
     * public override func $getJavaRef(): Java_CFFI_JavaEntity {
     *     return $javaref
     * }
     */
    void InsertJavaRefGetterWithBody(ClassDecl& decl);

    /**
     * before [ctor]:
     *   init(a1: A, a2: B, ..., an: N) { // empty body generated on the previous compilation steps
     *   }
     * --------
     * after:
     *   init(a1: A, a2: B, ..., an: N) {
     *       this({
     *           let jniEnv = Java_CFFI_get_env()
     *           Java_CFFI_newJavaObject(jniEnv, typeSignature, "(<argsSignature>)V", [
     *               Java_CFFI_JavaEntity(a1),
     *               Java_CFFI_JavaEntity(a2),
     *               ...,
     *               Java_CFFI_JavaEntity(an)])
     *       })
     *   }
     */
    void DesugarJavaMirrorConstructor(FuncDecl& ctor, FuncDecl& generatedCtor);

    FuncParam& PushEnvParams(std::vector<OwnedPtr<FuncParam>>& params, const std::string& name = "env");

    FuncParam& PushObjParams(std::vector<OwnedPtr<FuncParam>>& params, const std::string& name = "obj");

    OwnedPtr<AST::FuncParam> CreateJniEnvParam(const std::string& name = "$jnienv");

    OwnedPtr<AST::FuncParam> CreateJniJobjectOrJclassParam(const std::string& name = "$obj");

    OwnedPtr<AST::FuncParam> CreateRegistryIdParam(const std::string& name = "$regId");

    FuncParam& PushSelfParams(std::vector<OwnedPtr<FuncParam>>& params, std::string name = "self");

    OwnedPtr<CallExpr> GetFwdClassInstance(OwnedPtr<RefExpr> paramRef, Decl& fwdClassDecl);

    bool FillMethodParamsByArg(std::vector<OwnedPtr<FuncParam>>& params, std::vector<OwnedPtr<FuncArg>>& callArgs,
        FuncDecl& funcDecl, OwnedPtr<FuncParam>& arg, FuncParam& jniEnvPtrParam, Ptr<Ty> actualTy);

    OwnedPtr<Decl> GenerateNativeMethod(FuncDecl& sampleMethod, Decl& decl,
        const GenericConfigInfo* genericConfig = nullptr);

    void GenerateFuncParamsForNativeDeleteCjObject(
        Decl& decl, std::vector<OwnedPtr<FuncParam>>& params, FuncParam*& jniEnv, OwnedPtr<Expr>& selfRef);

    OwnedPtr<Decl> GenerateNativeFuncDeclBylambda(Decl& decl, OwnedPtr<LambdaExpr>& wrappedNodesLambda,
        std::vector<OwnedPtr<FuncParamList>>& paramLists, FuncParam& jniEnvPtrParam, Ptr<Ty>& retTy,
        std::string funcName);

    OwnedPtr<Decl> GenerateNativeFuncDeclBylambda(OwnedPtr<LambdaExpr>& wrappedNodesLambda,
        std::vector<OwnedPtr<FuncParamList>>& paramLists, FuncParam& jniEnvPtrParam, Ptr<Ty>& retTy,
        std::string funcName, Ptr<File>& curFile, std::string moduleName, std::string fullPackageName);

    std::string GetJniMethodName(const FuncDecl& method, const std::string* genericActualName = nullptr);

    std::string GetJniTupleItemName(const Ptr<TupleTy>& tupleTy, Package& pkg, size_t index);

    std::string GetJniMethodNameForProp(const PropDecl& propDecl, bool isSet,
        const std::string* genericActualName = nullptr) const;

    std::string GetJniInitCjObjectFuncName(const FuncDecl& ctor, bool isGeneratedCtor,
        const std::string* genericActualName = nullptr);
    std::string GetJniInitCjObjectFuncName(const Ptr<TupleTy>& tupleTy, Package& pkg);

    std::string GetJniInitCjObjectFuncNameForVarDecl(const VarDecl& ctor) const;

    std::string GetJniDeleteCjObjectFuncName(const Decl& decl) const;

    std::string GetJniDetachCjObjectFuncName(const Decl& decl) const;

    OwnedPtr<FuncDecl> CreateNativeFunc(const std::string& funcName,
        std::vector<OwnedPtr<FuncParam>> params, Ptr<Ty> retTy, std::vector<OwnedPtr<Node>> nodes);

    /**
     * ```cangjie
     * @C public func Java_<Type>_deleteCJObject(env: JNIEnv_ptr, clazz: jclass, regId: jlong): Unit {
     *      withExceptionHandling(env) {
     *          Java_CFFI_removeFromRegistry(regId)
     *     }
     *   }
     * ```
     */
    OwnedPtr<Decl> GenerateNativeDeleteCjObjectFunc(Decl& decl);

    OwnedPtr<Decl> GenerateCJMappingNativeDetachCjObjectFunc(ClassDecl& fwdDecl, ClassDecl& classDecl);

    // A helper function to for the ctor of Enum.
    void GenerateNativeInitCJObjectEnumCtor(AST::EnumDecl& enumDecl);

    /**
     * @C public func Java_<enum_identifier>_initCJObject(env: JNIEnv_ptr, obj: jobject): jlong {
           return withExceptionHandling(env, { =>
               return Java_CFFI_put_to_registry_1(Enum.VarDecl)
           })
     * }
     */
    OwnedPtr<Decl> GenerateNativeInitCjObjectFuncForEnumCtorNoParams(AST::EnumDecl& enumDecl, AST::VarDecl& ctor);

    /**
     * when arg ClassLikeDecl is true(e.g. calss decl)::
     *
     * @C public func Java_<type_signature>_initCJObject{mangled}(env: JNIEnv_ptr, obj: jobject, args...): jlong {
     *     registry.put(
     *         <type>(Java_CFFI_newGlobalReference(env, Java_CFFI_JavaEntity(JOBJECT, obj), true), args...)
     *     )
     * }
     * when arg ClassLikeDecl is false (e.g. struct decl):
     *
     * @C public func Java_<type_signature>_initCJObject{mangled}(env: JNIEnv_ptr, obj: jobject, args...): jlong {
     *     registry.put(
     *         <type>(args...)
     *     )
     * }
     */
    OwnedPtr<Decl> GenerateNativeInitCjObjectFunc(FuncDecl& ctor,
        bool isClassLikeDecl,
        bool isOpenClass = false,
        Ptr<FuncDecl> fwdCtor = nullptr,
        const GenericConfigInfo* genericConfig = nullptr);

    OwnedPtr<Decl> GenerateNativeInitCjObjectFunc(const Ptr<TupleTy>& tuple, Package& pkg);

    /**
     * for func [fun]:
     *     func foo(args): Ret
     *
     * the following will be generated:
     *     func foo(args): Ret {
     *         *UnwrapJavaEntity*(
     *             Java_CFFI_callMethod_raw(
     *                 Java_CFFI_get_env(),
     *                 this.javaref, // or getJavaref if mirror is an interface
     *                 typeSignature, "foo", "(<argsSignature>)Ret",
     *                 [Java_CFFI_JavaEntity(args[0]), ... Java_CFFI_JavaEntity(args[n])]
     *         )
     *     }
     *
     * where *UnwrapJavaEntity* - generated unwrapper for Ret type value.
     */
    void DesugarJavaMirrorMethod(FuncDecl& fun, ClassLikeDecl& mirror, GenericConfigInfo *config = nullptr);

    /**
     * used in DesugarJavaMirrorMethod for method's body generation
     *
     */
    void AddJavaMirrorMethodBody(ClassLikeDecl& mirror,
        FuncDecl& fun,
        OwnedPtr<Expr> javaRefCall,
        GenericConfigInfo *config = nullptr);

    /**
     * for prop [prop]:
     *   mut prop p: Ret
     *
     * the following will be generated:
     *     mut prop p: Ret {
     *         get() {
     *             *UnwrapJavaEntity*(
     *                 Java_CFFI_getField_raw(
     *                     Java_CFFI_get_env(), this.javaref, typeSignature, "p", "Ret"
     *             ))
     *         }
     *         set(v) {
     *             Java_CFFI_setField_raw(
     *                 Java_CFFI_get_env(),
     *                 this.javaref, typeSignature, "p", "Ret", Java_CFFI_JavaEntity(v)
     *             )
     *         }
     *     }
     */
    void DesugarJavaMirrorProp(PropDecl& prop);

    void InsertJavaMirrorPropGetter(PropDecl& prop);
    void InsertJavaMirrorPropSetter(PropDecl& prop);

    /**
     * For CType ty, ty is returned. For mirrors, impls and CJMapping jobject is returned
     */
    Ptr<Ty> GetJNITy(Ptr<Ty> ty);

    /**
     * Inserts constructor of form `JString(String)`.
     * The operation consists of two steps:
     * 1) Insert constructor stub (constructor with empty body): [doStub] = `true`
     * 2) Fills generated constructor with actual body: [doStub] = `false`
     *
     * public init(s: String) {
     *   super(Java_CFFI_CangjieStringToJava(env, s))
     * }
     */
    void InsertJStringOfStringCtor(ClassDecl& decl, bool doStub);

    void GenerateForCJStructOrClassTypeMapping(const File &file, AST::Decl* decl);
    void GenerateForCJEnumMapping(AST::EnumDecl& enumDecl);
    void GenerateForCJExtendMapping(AST::ExtendDecl& extendDecl);

    /**
     * for a cj-mapping interface:
     *
     * public interface CJMappingInterface {
     *   public func foo() : Unit {...}
     * }
     *
     * the following forward class and native method will be generated:
     *
     * class CJMappingInterface_fwd <: CJMappingInterface { // Attribute::CJ_MIRROR_JAVA_INTERFACE_FWD
     *     public let javaref: Java_CFFI_JavaEntity
     *
     *     public init(ref: Java_CFFI_JavaEntity) {...}
     *
     *     public func foo(): Unit {
     *         jniCall("Java/A", "foo", "()V", [])
     *     }
     *
     *     public func foo_default_impl(): Unit {...} // Attribute::JAVA_CJ_MAPPING_INTERFACE_DEFAULT
     * }
     *
     * @C
     * public func Java_CJMappingInterface_1fwd_foo_1default_1impl(env, _: jclass, javaref: jobject) {
     *     return CJMappingInterface_fwd(javaref).foo_default_impl()
     * }
     */
    void GenerateForCJInterfaceMapping(File& file, AST::InterfaceDecl& interfaceDecl);
    void GenerateNativeForCJInterfaceMapping(AST::ClassDecl& classDecl);
    void GenerateInterfaceFwdclassBody(AST::ClassDecl& fwdclassDecl,
        AST::InterfaceDecl& interfaceDecl,
        GenericConfigInfo* config = nullptr);
    OwnedPtr<FuncDecl> GenerateInterfaceFwdclassMethod(AST::ClassDecl& fwdclassDecl,
        FuncDecl& interfaceFuncDecl,
        GenericConfigInfo *config = nullptr);
    OwnedPtr<FuncDecl> GenerateInterfaceFwdclassDefaultMethod(AST::ClassDecl& fwdclassDecl,
        FuncDecl& interfaceFuncDecl,
        GenericConfigInfo *config = nullptr);

    void GenerateForCJOpenClassMapping(AST::ClassDecl& classDecl);
    void GenerateClassFwdclassBody(AST::ClassDecl& fwdclassDecl,
        AST::ClassDecl& classDecl,
        std::vector<std::pair<Ptr<FuncDecl>,
        Ptr<FuncDecl>>>& pairCtors);
    void InsertJavaObjectControllerVarDecl(ClassDecl& fwdClassDecl, ClassDecl& classDecl);
    void InsertOverrideMaskVar(AST::ClassDecl& fwdclassDecl);
    OwnedPtr<FuncDecl> GenerateFwdClassCtor(ClassDecl& fwdDecl, ClassDecl& classDecl, FuncDecl& oriCtorDecl);
    void InsertAttachCJObject(ClassDecl& fwdDecl, ClassDecl& classDecl);
    OwnedPtr<FuncDecl> GenerateFwdClassMethod(ClassDecl& fwdDecl,
        ClassDecl& classDecl,
        FuncDecl& oriMethodDecl,
        int index);
    OwnedPtr<ClassDecl> InitInterfaceFwdClassDecl(AST::InterfaceDecl& interfaceDecl);
    OwnedPtr<StructDecl> CreateHelperStructDecl(const Ptr<TupleTy>& tupleTy, Package& pkg);
    void GenerateNativeItemFunc(const Ptr<TupleTy>& tupleTy, Package& pkg);

    /**
     * Add this. for interface fwdclass default method that call self method,
     * and replace generic ty to instance ty by genericConfig.
     * from
     * interface B {
     * func test() {test2()}
     * func test2()
     * }
     * to
     * class B_fwd {
     * func test() {this.test2()} // add this.
     * func test2(){}
     * }
     */
    OwnedPtr<AST::MemberAccess> GenThisMemAcessForSelfMethod(Ptr<FuncDecl> fd,
        Ptr<InterfaceDecl> interfaceDecl,
        GenericConfigInfo* genericConfig);
    void PreGenerateInCJMapping(File& file);

    /**
     * config (Int32) -> Int32, will generate as follow:
     *
     * public func getInt32ToInt32CjLambda(a: jobject) : (Int32) -> Int32 {
     *  let javaref = Java_CFFI_JavaEntityJobject(a)
     *  let cjLambda = { b:Int32 =>
     *      let env = Java_CFFI_get_env()
     *      let methodId = Java_CFFI_MethodIDConstr(env, Java_CFFI_ClassInit(env, "cj/IntWInt"), "call",
     *      Java_CFFI_parseMethodSignature("(I)I")); let result = Java_CFFI_callVirtualMethod(env, javaref, methodId,
     *      [Java_CFFI_JavaEntity(b)], Java_CFFI_JavaCallNestInit(1)) Java_CFFI_unwrapJavaEntityAsValue<Int32>(result)
     *      }
     *  cjLambda
     *  }
     *
     * @C
     * public func Java_cj_IntWInt_00024BoxIntWInt_intWIntImpl(env: JNIEnv_ptr, _: jclass, self: jlong, arg: Int32):
     * Int32 {
     *       withExceptionHandling(env) {
     *      let v = Java_CFFI_getFromRegistry<(Int32) -> Int32>(env, self)
     *       let r = v(arg)
     *       r
     *       }
     *   }
     *
     * @C
     * public func Java_cj_IntWInt_00024BoxIntWInt_deleteIntWIntCJObject(env: JNIEnv_ptr, _: jclass, self:jlong): Unit {
     *       withExceptionHandling(env) {
     *           Java_CFFI_deleteCJObjectOneWay<(Int32) -> Int32>(env, self)
     *      }
     *   }
     */
    void GenerateLambdaGlueCode(File& file);

    /**
     * config (Int32) -> Int32, will generate as follow:
     *  let cjLambda = { b:Int32 =>
     *      let env = Java_CFFI_get_env()
     *      let methodId = Java_CFFI_MethodIDConstr(env, Java_CFFI_ClassInit(env, "cj/IntWInt"), "call",
     *      Java_CFFI_parseMethodSignature("(I)I")); let result = Java_CFFI_callVirtualMethod(env, javaref, methodId,
     *      [Java_CFFI_JavaEntity(b)], Java_CFFI_JavaCallNestInit(1)) Java_CFFI_unwrapJavaEntityAsValue<Int32>(result)
     *      }
     *  cjLambda
     *  }
     */
    OwnedPtr<LambdaExpr> GenerateLambdaExpr(File& file, LambdaPattern& pattern, FuncParam& funcParam);

    /**
     * check whether exist lambdaDecl by function param ty.
     */
    Ptr<FuncDecl> CheckCjLambdaDeclByTy(Ptr<Ty> ty);

    /**
     * generate callexpr for getInt32ToInt32CjLambda() if param ty is (Int32)->Int32.
     */
    OwnedPtr<CallExpr> CreateGetCJLambdaCallExpr(OwnedPtr<RefExpr> callResRef, Ptr<Ty> ty, const Decl& outerDecl);


    /**
     * generate java interface functional call() method's native function decl.
     */
    OwnedPtr<Decl> GenerateCallImplNativeMethod(File& file, LambdaPattern& lambdaPattern);
    Ptr<FuncTy> GetLambdaFuncTy(LambdaPattern& lambdaPattern);
    Ptr<Decl> GetLambdaTmpDecl(File& file, std::string javaClassName, std::string fullPackGeName);
    std::string GetLambdaCallImplJniMethodName(Decl& decl);

    ImportManager& importManager;
    TypeManager& typeManager;
    Utils utils;
    DiagnosticEngine& diag;
    const BaseMangler& mangler;
    InteropLibBridge lib;
    const std::optional<std::string>& javaCodeGenPath;
    const std::string& outputLibPath;
    std::unordered_set<Ptr<Ty>> tupleConfigs;

    /**
     * Top-level declarations generated during desugaring. Should be added at the end of file desugaring
     */
    std::vector<OwnedPtr<Decl>> generatedDecls;

    // contains the member signatures of structs.
    const std::unordered_map<Ptr<const AST::InheritableDecl>, MemberMap>& memberMap;
    std::map<std::string, Ptr<FuncDecl>> lambdaConfUtilFuncs;
    bool isInitLambdaUtilFunc = false;
};

} // namespace Cangjie::Interop::Java

#endif // CANGJIE_SEMA_NATIVE_FFI_JAVA_DESUGAR_MANAGER
