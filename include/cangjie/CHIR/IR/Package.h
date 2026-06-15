// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_PACKAGE_H
#define CANGJIE_CHIR_PACKAGE_H

#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/EnumDef.h"
#include "cangjie/CHIR/IR/Type/ExtendDef.h"
#include "cangjie/CHIR/IR/Type/StructDef.h"

#include <string>
#include <vector>

namespace Cangjie::CHIR {
class Package {
    explicit Package(const std::string& name);
    ~Package() = default;
    friend class CHIRContext;
    friend class CHIRBuilder;

public:
    enum class AccessLevel : uint8_t {
        INVALID = 0,
        INTERNAL,
        PROTECTED,
        PUBLIC
    };
    // ===--------------------------------------------------------------------===//
    // Base Infomation
    // ===--------------------------------------------------------------------===//
    void Dump() const;
    std::string ToString() const;
    std::string GetName() const;
    AccessLevel GetPackageAccessLevel() const;
    void SetPackageAccessLevel(const AccessLevel& level);

    // ===--------------------------------------------------------------------===//
    // Global Var API
    // ===--------------------------------------------------------------------===//
    void AddGlobalVar(GlobalVar* item);
    GlobalVar* TryGetGlobalVar(const std::string& identifier);

    // including:
    // 1. imported global var, exclude src code imported global var
    std::vector<GlobalVar*> GetGlobalVarsWithoutInit() const;
    // including:
    // 1. global var in current package
    // 2. src code imported global var
    std::vector<GlobalVar*> GetGlobalVarsWithInit(bool includeSrcCodeImported = true) const;
    std::vector<GlobalVar*> GetGlobalVars() const;
    void SetAllGlobalVars(std::vector<GlobalVar*>&& vars);

    // ===--------------------------------------------------------------------===//
    // Global Function API
    // ===--------------------------------------------------------------------===//
    void AddGlobalFunc(Function* item);
    Function* TryGetGlobalFunc(const std::string& identifier);
    
    // including:
    // 1. imported function, excluding src code imported function
    // 2. pure abstract function, including declared in current package and imported package
    std::vector<Function*> GetGlobalFuncsWithoutBody(bool includePureAbstract = false) const;
    // including:
    // 1. global function in current package, excluding pure abstract function
    // 2. src code imported function
    // 3. instantiated function but its generic decl is from imported package
    std::vector<Function*> GetGlobalFuncsWithBody(bool includeSrcCodeImported = true) const;
    std::vector<Function*> GetGlobalFunctions(bool includePureAbstract = false) const;
    void SetAllGlobalFuncs(std::vector<Function*>&& funcs);

    Function* GetPackageInitFunc() const;
    void SetPackageInitFunc(Function* func);

    void SetPackageLiteralInitFunc(Function* func);
    Function* GetPackageLiteralInitFunc() const;

    // ===--------------------------------------------------------------------===//
    // StructDef API
    // ===--------------------------------------------------------------------===//
    void AddStruct(StructDef* item);
    std::vector<StructDef*> GetStructs() const;
    void SetStructs(std::vector<StructDef*>&& s);

    void AddImportedStruct(StructDef* item);
    std::vector<StructDef*> GetImportedStructs() const;
    void SetImportedStructs(std::vector<StructDef*>&& s);

    std::vector<StructDef*> GetAllStructDef() const;
    StructDef* TryGetStructDef(const std::string& identifier);

    // ===--------------------------------------------------------------------===//
    // ClassDef API
    // ===--------------------------------------------------------------------===//
    void AddClass(ClassDef* item);
    std::vector<ClassDef*> GetClasses() const;
    void SetClasses(std::vector<ClassDef*>&& items);

    void AddImportedClass(ClassDef* item);
    std::vector<ClassDef*> GetImportedClasses() const;
    void SetImportedClasses(std::vector<ClassDef*>&& s);

    std::vector<ClassDef*> GetAllClassDef() const;
    ClassDef* TryGetClassDef(const std::string& identifier);

    // ===--------------------------------------------------------------------===//
    // EnumDef API
    // ===--------------------------------------------------------------------===//
    void AddEnum(EnumDef* item);
    std::vector<EnumDef*> GetEnums() const;
    void SetEnums(std::vector<EnumDef*>&& s);

    void AddImportedEnum(EnumDef* item);
    std::vector<EnumDef*> GetImportedEnums() const;
    void SetImportedEnums(std::vector<EnumDef*>&& s);

    std::vector<EnumDef*> GetAllEnumDef() const;
    EnumDef* TryGetEnumDef(const std::string& identifier);

    // ===--------------------------------------------------------------------===//
    // ExtendDef API
    // ===--------------------------------------------------------------------===//
    void AddExtend(ExtendDef* item);
    std::vector<ExtendDef*> GetExtends() const;
    void SetExtends(std::vector<ExtendDef*>&& items);

    void AddImportedExtend(ExtendDef* item);
    std::vector<ExtendDef*> GetImportedExtends() const;
    void SetImportedExtends(std::vector<ExtendDef*>&& items);

    std::vector<ExtendDef*> GetAllExtendDef() const;
    ExtendDef* TryGetExtendDef(const std::string& identifier);

    // ===--------------------------------------------------------------------===//
    // Others API
    // ===--------------------------------------------------------------------===//
    std::vector<CustomTypeDef*> GetAllCustomTypeDef() const;
    std::vector<CustomTypeDef*> GetCurPkgCustomTypeDef() const;
    std::vector<CustomTypeDef*> GetAllImportedCustomTypeDef() const;
private:
    std::string name;                                  // full package name, like "std.core"
    AccessLevel pkgAccessLevel{AccessLevel::INVALID};  // public/internal/protected, get from AST

    // imported decls
    std::vector<StructDef*> importedStructs;
    std::vector<ClassDef*> importedClasses;
    std::vector<EnumDef*> importedEnums;
    std::vector<ExtendDef*> importedExtends;

    // decls in current package
    std::vector<GlobalVar*> globalVars;
    std::vector<Function*> globalFuncs;
    std::vector<StructDef*> structs;
    std::vector<ClassDef*> classes;
    std::vector<EnumDef*> enums;
    std::vector<ExtendDef*> extends;
    Function* packageInitFunc = nullptr;
    Function* packageLiteralInitFunc = nullptr; // global literals init function in one package
};

} // namespace Cangjie::CHIR

#endif
