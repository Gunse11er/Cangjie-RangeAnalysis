// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CUSTOMTYPE_H
#define CANGJIE_CUSTOMTYPE_H

#include "cangjie/CHIR/IR/Value/Value.h"

#include <cstddef>
#include <optional>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <set>
#include <vector>

namespace Cangjie::CHIR {
class ClassDef;
class ExtendDef;

struct MemberVarInfo {
    std::string name;
    std::string rawMangledName;
    Type* type = nullptr;
    AttributeInfo attributeInfo;
    DebugLocation loc;
    AnnoInfo annoInfo;
    Function* initializerFunc = nullptr; /**< Function with initializer evaluation if any */
    const CustomTypeDef* outerDef = nullptr;
    bool TestAttr(Attribute attr) const
    {
        return attributeInfo.TestAttr(attr);
    }

    bool IsImmutable() const
    {
        return attributeInfo.TestAttr((Attribute::READONLY)) || attributeInfo.TestAttr(Attribute::CONST);
    }
};

enum CustomDefKind : uint8_t {
    TYPE_STRUCT,
    TYPE_ENUM,
    TYPE_CLASS, // include class, interface
    TYPE_EXTEND
};

class CustomTypeDef : public Base {
    friend class GlobalVar;
    friend class Function;
    friend class CustomType;
    friend class CustomDefTypeConverter;
    friend class CHIRDeserializer;

public:
    // ===--------------------------------------------------------------------===//
    // Base Infomation
    // ===--------------------------------------------------------------------===//
    CustomDefKind GetCustomKind() const;
    std::string GetIdentifier() const;
    std::string GetIdentifierWithoutPrefix() const;
    std::string GetSrcCodeIdentifier() const;
    std::string GetPackageName() const;

    virtual Type* GetType() const;
    virtual void SetType(CustomType& ty) = 0;

    virtual std::vector<GenericType*> GetGenericTypeParams() const;
    bool IsGenericDef() const;

    void Dump() const;
    std::string ToString() const;

    bool IsClass() const;
    /** @brief is class or interface */
    bool IsClassLike() const;
    bool IsEnum() const;
    bool IsExtend() const;
    bool IsInterface() const;
    bool IsStruct() const;

    // ===--------------------------------------------------------------------===//
    // Super Parent
    // ===--------------------------------------------------------------------===//
    void AddImplementedInterfaceTy(ClassType& interfaceTy);
    std::vector<ClassDef*> GetImplementedInterfaceDefs() const;
    std::vector<ClassType*> GetImplementedInterfaceTys() const;
    size_t GetImplementedInterfacesNum() const;

    /**
     * @brief super types in current def, including class and interface, excluding super types' super types
     * e.g. class A <: C & I1 {}; class C <: I2 {}
     * then super types in class A are C and I1, not including I2
     *
     * @return super types, but we don't guarantee the order
     */
    std::vector<ClassType*> GetSuperTypesInCurDef() const;

    /** @brief visit super def and super def's all extend defs which meet the condition,
     *  and get their super classes or interfaces recursively, not including current def's extend defs
     *
     *  interface I1 {}
     *  interface I2 <: I1 {}
     *  class C1 <: I2
     *
     *  interface I3 {}
     *  interface I4 <: I3 {}
     *  extend C1 <: I4
     *
     *  interface I5 {}
     *  class C2 <: C1 & I5
     *
     *  interface I6 {}
     *  extend C2 <: I6 {}
     *
     *  so the result of `C2.GetSuperTypesRecusively()` is {I1 ~ I5, C1}, not including I6
     *
     * @return super types, but we don't guarantee the order
     */
    std::vector<ClassType*> GetSuperTypesRecusively(CHIRBuilder& builder) const;

    // ===--------------------------------------------------------------------===//
    // Member Function
    // ===--------------------------------------------------------------------===//
    void AddMethod(class Function* method);
    std::vector<Function*> GetMethods() const;
    // you need to update vtable by yourself, after setting methods
    void SetMethods(const std::vector<Function*>& items);

    /**
     * @brief Retrieves the expected function based on the given name and type.
     *
     * @param funcName The name of the function to be retrieved.
     * @param funcType The type of the function to be retrieved.
     * @param isStatic Indicates whether the function is static.
     * @param replaceTable A map for replacing generic types with specific types.
     * @param funcInstTypeArgs A vector to store the function instance type arguments.
     * @param builder The CHIR builder used for building the function.
     * @param checkAbstractMethod Indicates whether to check for an abstract method.
     * @return The expected function.
     */
    Function* GetExpectedFunc(const std::string& funcName, FuncType& funcType, bool isStatic,
        std::unordered_map<const GenericType*, Type*> replaceTable,
        std::vector<Type*>& funcInstTypeArgs, CHIRBuilder& builder, bool checkAbstractMethod) const;

    // ===--------------------------------------------------------------------===//
    // Member Var
    // ===--------------------------------------------------------------------===//
    void AddStaticMemberVar(class GlobalVar* variable);
    std::vector<GlobalVar*> GetStaticMemberVars() const;
    void SetStaticMemberVars(const std::vector<GlobalVar*>& vars);

    /**
    * @brief Add member into non-static members.
    *
    * @param variable instance member var
    */
    void AddInstanceVar(MemberVarInfo variable);
    /**
    * @brief get instance member var quantity
    *
    * @return count from super class's instance member var
    */
    size_t GetAllInstanceVarNum() const;
    /**
    * @brief get instance member var
    *
    * @param index count from super class's instance member var
    * @return instance member var
    */
    MemberVarInfo GetInstanceVar(size_t index) const;
    /**
    * @brief get all instance member vars
    *
    * @return including super class's and current def's instance member vars
    */
    std::vector<MemberVarInfo> GetAllInstanceVars() const;

    /**
    * @brief get instance member var quantity
    *
    * @return only count current def's instance member var, not including super class's
    */
    size_t GetDirectInstanceVarNum() const;
    MemberVarInfo GetDirectInstanceVar(size_t index) const;
    std::vector<MemberVarInfo> GetDirectInstanceVars() const;
    void SetDirectInstanceVars(const std::vector<MemberVarInfo>& vars);

    Function* GetVarInitializationFunc() const;
    void SetVarInitializationFunc(Function* func);

    // ===--------------------------------------------------------------------===//
    // Annotation
    // ===--------------------------------------------------------------------===//
    AnnoInfo GetAnnoInfo() const;
    void SetAnnoInfo(const AnnoInfo& info);

    // ===--------------------------------------------------------------------===//
    // Vtable
    // ===--------------------------------------------------------------------===//
    /**
    * @brief add virtual function to vtable
    *
    * @param srcClassTy a class which function belongs to
    * @param info virtual function info
    */
    void AddVtableItem(ClassType& srcClassTy, VirtualMethodInfo&& info);
    const VTableInDef& GetDefVTable() const;
    VTableInDef& GetModifiableDefVTable();
    void SetVTable(VTableInDef&& table);

    /**
    * @brief update virtual function in vtable
    *
    * @param srcClassTy a class which function belongs to
    * @param index virtual function's index
    * @param newFunc new virtual function pointer
    * @param newParentTy new virtual function's parent class, `nullptr` means not changed
    * @param newName new src code name, empty means not changed
    */
    void UpdateVtableItem(ClassType& srcClassTy,
        size_t index, Function* newFunc, Type* newParentTy = nullptr, const std::string newName = "");

    /**
    * @brief get virtual function's index in vtable, there may be many results, because for one func signature,
    *        we can get many results in different src parent class. Because in different class's vtable,
    *        the offset is different, so we need to store all results and left them to other function to judge,
    *        we don't have enough infomation to judge which one is best here
    *
    * @param funcCallType function name and type
    * @param replaceTable an auxiliary map
    * @param builder CHIR builder
    */
    std::vector<VTableSearchRes> GetFuncIndexInVTable(const FuncCallType& funcCallType,
        std::unordered_map<const GenericType*, Type*>& replaceTable, CHIRBuilder& builder) const;

    // ===--------------------------------------------------------------------===//
    // Extra Information
    // ===--------------------------------------------------------------------===//
    const std::vector<ExtendDef*>& GetExtends() const;
    void AddExtend(ExtendDef& extend);

    /**
    * @brief if current def is instantiated decl, we store its generic decl
    *
    * @return its generic decl
    */
    CustomTypeDef* GetGenericDecl() const;
    void SetGenericDecl(CustomTypeDef& decl);

    bool CanBeInherited() const;

protected:
    explicit CustomTypeDef(
        std::string srcCodeIdentifier, const std::string& identifier, const std::string& pkgName, CustomDefKind kind)
        : kind(kind), srcCodeIdentifier(srcCodeIdentifier),
          identifier(identifier),
          packageName(pkgName)
    {
    }
    virtual ~CustomTypeDef() = default;
    virtual std::string CustomTypeDefTitleToString() const;
    std::string GenericInsArgsToString(const CustomType& ty) const;
    std::string ParentToString() const;
    std::string CommentToString() const;
    virtual std::string AddExtraComment() const;
    virtual std::string LocalVarToString() const;
    std::string StaticVarToString() const;
    std::string MethodToString() const;
    std::string VTableToString() const;

protected:
    CustomDefKind kind : 8;
    std::string srcCodeIdentifier; /**< used for srcCodeIdentifier */
    std::string identifier;        /**< used for identifier */
    std::string packageName;       /**< package where this type defined by user */
    CustomType* type = nullptr;          /**< Type */
    /** original generic decl which current instantiated decl derives from */
    CustomTypeDef* genericDecl = nullptr;
    std::vector<Function*> methods;               /**< non-abstract member methods */
    std::vector<ClassType*> implementedInterfaceTys; /**< implemented interfaces */
    std::vector<MemberVarInfo> instanceVars;         /**< local member variables */
    std::vector<GlobalVar*> staticVars;       /**< static member variables */
    AnnoInfo annoInfo;                            /**< struct/class/enum annoInfo */
    VTableInDef vtable;
    std::vector<ExtendDef*> extends;
    Function* varInitializationFunc = nullptr; /**< Function for initializing instance variables with initializers */
};
} // namespace Cangjie::CHIR
#endif
