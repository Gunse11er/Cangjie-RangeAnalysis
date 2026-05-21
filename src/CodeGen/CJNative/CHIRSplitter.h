// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CODEGEN_CHIRSPLITTER_H
#define CANGJIE_CODEGEN_CHIRSPLITTER_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Cangjie {
namespace CHIR {
class CustomTypeDef;
class ClassDef;
class EnumDef;
class StructDef;
class ExtendDef;
class GlobalVar;
class Value;
class FuncBase;
class Func;
class ImportedFunc;
}
namespace CodeGen {
class CGPkgContext;

struct ChirTypeDefCmp {
    bool operator()(const CHIR::CustomTypeDef* lhs, const CHIR::CustomTypeDef* rhs) const;
};

struct ChirValueCmp {
    bool operator()(const CHIR::Value* lhs, const CHIR::Value* rhs) const;
};

struct SubCHIRPackage {
    bool mainModule = false;
    std::size_t subCHIRPackageIdx;
    std::size_t exprNumInChirFuncs;
    std::size_t splitNum;
    std::set<CHIR::CustomTypeDef*, ChirTypeDefCmp> chirCustomDefs;
    std::set<CHIR::GlobalVar*, ChirValueCmp> chirGVs;
    std::set<CHIR::Func*, ChirValueCmp> chirFuncs;
    std::set<CHIR::ImportedFunc*, ChirValueCmp> chirForeigns;
    std::set<CHIR::ImportedFunc*, ChirValueCmp> chirImportedCFuncs;

    explicit SubCHIRPackage(std::size_t splitNum);
    void Clear();
};

class CHIRSplitter {
public:
    explicit CHIRSplitter(const CGPkgContext& cgPkgCtx);

    std::vector<SubCHIRPackage> SplitCHIRPackage();

private:
    struct SubCHIRPackagesCache {
        std::optional<std::size_t> splitNum = std::nullopt;
        std::map<std::string, std::size_t> classesCache;
        std::map<std::string, std::size_t> enumsCache;
        std::map<std::string, std::size_t> structsCache;
        std::map<std::string, std::size_t> extendDefCache;
        std::map<std::string, std::size_t> gvsCache;
        std::map<std::string, std::size_t> funcsCache;
        std::map<std::string, std::size_t> foreignsCache;
        std::map<std::string, std::size_t> importedCFuncsCache;
    };

    void CalcSplitsNum();

    void SplitCHIRFuncs(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIRClasses(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIREnums(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIRStructs(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIRExtends(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIRGlobalVars(std::vector<SubCHIRPackage>& subCHIRPackages);
    void SplitCHIRImportedCFuncs(std::vector<SubCHIRPackage>& subCHIRPackages);

    unsigned long FindIdxInCache(const std::string& key);

    void LoadSubCHIRPackagesInfo();
    void SaveSubCHIRPackagesInfo();

    const CGPkgContext& cgPkgCtx;
    std::size_t splitNum;
    std::size_t index;

    SubCHIRPackagesCache subCHIRPackagesCache;
};
} // namespace CodeGen
} // namespace Cangjie
#endif