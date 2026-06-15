// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_GLOBALVARINITIALIZER_H
#define CANGJIE_CHIR_GLOBALVARINITIALIZER_H

#include "cangjie/CHIR/AST2CHIR/TranslateASTNode/Translator.h"
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/CHIR/AST2CHIR/Utils.h"

namespace Cangjie::CHIR {
//                           ordered file                 ordered var decl
using OrderedDecl = std::pair<std::vector<Ptr<AST::File>>, std::vector<Ptr<AST::Decl>>>;

class GlobalVarInitializer {
public:
    explicit GlobalVarInitializer(Translator& trans, const ImportManager& importManager,
        std::vector<Function*>& initFuncsForConstVar, bool enableIncre)
        : builder(trans.builder),
          globalSymbolTable(trans.globalSymbolTable),
          opts(trans.opts),
          importManager(importManager),
          initFuncsForConstVar(initFuncsForConstVar),
          trans(trans),
          enableIncre(enableIncre)
    {
    }

    /**
    * @brief generate global var init function
    *
    * @param pkg AST package
    * @param initOrder ordered global var decls
    */
    void Run(const AST::Package& pkg, const InitOrder& initOrder);

private:
    void CreatePackageInit(const AST::Package& curPackage, const InitOrder& initOrder);
    void CreatePackageLiteralInit(const AST::Package& curPackage, const InitOrder& initOrder);
    inline std::pair<Function*, Block*> PreparePackageInit(const AST::Package& curPackage);
    inline std::pair<Function*, Block*> PreparePackageLiteralInit(const AST::Package& curPackage);
    void InsertInitializerIntoPackageInitializer(Function& init, Function& packageInit);
    Function* TranslateSingleInitializer(const AST::VarDecl& decl);
    bool IsIncrementalNoChange(const AST::VarDecl& decl) const;
    Function* TranslateInitializerToFunction(const AST::VarDecl& decl);
    Function* TranslateIncrementalNoChangeVar(const AST::VarDecl& decl);
    Ptr<Value> GetGlobalVariable(const AST::VarDecl& decl);
    template <typename T, typename... Args> Ptr<Function> CreateGVInitFunc(const T& node, Args&& ... args) const;
    void RemoveInitializerForVarDecl(const AST::VarDecl& varDecl, Function& fileInit) const;
    void RemoveCommonInitializersReplacedWithSpecific(
        Function& fileInit, const std::vector<Ptr<const AST::Decl>>& decls) const;
    Ptr<Function> TryGetFileInitializer(const AST::File& file, const std::string& suffix = "");
    Ptr<Function> TranslateFileInitializer(const AST::File& file, const std::vector<Ptr<const AST::Decl>>& decls);
    Ptr<Function> TranslateFileLiteralInitializer(
        const AST::File& file, const std::vector<Ptr<const AST::Decl>>& decls);
    Function* TranslateVarWithPatternInitializer(const AST::VarWithPatternDecl& decl);
    Function* TranslateWildcardPatternInitializer(const AST::VarWithPatternDecl& decl);
    Function* TranslateTupleOrEnumPatternInitializer(const AST::VarWithPatternDecl& decl);
    void FillGVInitFuncWithApplyAndExit(const std::vector<Ptr<Value>>& varInitFuncs);
    void AddImportedPackageInit(const AST::Package& curPackage, const std::string& suffix = "");
    Ptr<Function> GetImportsInitFunc(const AST::Package& curPackage, const std::string& suffix = "");
    Ptr<Function> CreateImportsInitFunc(const AST::Package& curPackage, const std::string& suffix = "");
    void UpdateImportsInit(const AST::Package& curPackage, Function& importsInitFunc, const std::string& suffix = "");
    void AddGenericInstantiatedInit();
    Ptr<Function> GeneratePackageInitBase(const AST::Package& curPackage, const std::string& suffix = "");
    bool NeedVarLiteralInitFunc(const AST::Decl& decl);
    Function* TranslateVarInit(const AST::Decl& var);

    // Add methods for CJMP
    template<typename T>
    T* TryGetDeserialized(const std::string& mangledName)
    {
        // merging platform
        if (opts.IsCompilingCJMPSpecific()) {
            return TryGetFromCache<Value, T>(GLOBAL_VALUE_PREFIX + mangledName, trans.deserializedVals);
        }
        return nullptr;
    }

private:
    CHIRBuilder& builder;
    AST2CHIRNodeMap<Value>& globalSymbolTable;
    const GlobalOptions& opts;
    const ImportManager& importManager;
    std::vector<Function*>& initFuncsForConstVar;
    Translator& trans;
    bool enableIncre;
};

} // namespace Cangjie::CHIR

#endif
