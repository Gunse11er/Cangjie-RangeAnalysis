// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the sub-class DependencyGraph of ImportManager.
 */

#include "cangjie/Modules/ImportManager.h"

#include <string>
#include <vector>

#include "cangjie/AST/Match.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/Frontend/CompilerInstance.h"
#include "cangjie/Modules/ASTSerialization.h"
#include "cangjie/Modules/ModulesUtils.h"

using namespace Cangjie;
using namespace AST;

const std::vector<Ptr<AST::PackageDecl>>& ImportManager::DependencyGraph::GetDirectDependencyPackageDecls(
    const std::string& fullPackageName)
{
    auto cacheIter = cacheDirectDependencyPackageDecls.find(fullPackageName);
    if (cacheIter != cacheDirectDependencyPackageDecls.cend()) {
        return cacheIter->second;
    }
    std::vector<Ptr<AST::PackageDecl>> packageDecls;
    std::unordered_set<Ptr<AST::PackageDecl>> collected;
    std::queue<Ptr<AST::PackageDecl>> tmpQueue;
    for (auto& edge : GetEdges(fullPackageName)) {
        auto pkgDecl = cjoManager.GetPackageDecl(edge.first);
        if (pkgDecl == nullptr) {
            continue;
        }
        tmpQueue.push(pkgDecl);
    }
    while (!tmpQueue.empty()) {
        auto pkgDecl = tmpQueue.front();
        tmpQueue.pop();
        if (auto [_, succ] = collected.emplace(pkgDecl); succ) {
            packageDecls.emplace_back(pkgDecl);
        }
        if (!pkgDecl->srcPackage->isMacroPackage) {
            continue;
        }
        for (auto& reExportPackageName : pkgReExportMap[pkgDecl->srcPackage->fullPackageName]) {
            auto reExportPkgDecl = cjoManager.GetPackageDecl(reExportPackageName);
            CJC_NULLPTR_CHECK(reExportPkgDecl);
            if (!reExportPkgDecl->srcPackage->isMacroPackage && collected.count(reExportPkgDecl) == 0) {
                packageDecls.emplace_back(reExportPkgDecl);
                collected.emplace(reExportPkgDecl);
            } else if (reExportPkgDecl->srcPackage->isMacroPackage) {
                tmpQueue.push(reExportPkgDecl);
                collected.emplace(reExportPkgDecl);
            }
        }
    }
    auto [cacheIter1, _] = cacheDirectDependencyPackageDecls.emplace(fullPackageName, std::move(packageDecls));
    return cacheIter1->second;
}

const std::vector<Ptr<AST::PackageDecl>>& ImportManager::DependencyGraph::GetAllDependencyPackageDecls(
    const std::string& fullPackageName, bool includeMacroPkg)
{
    auto keyPair = std::make_pair(fullPackageName, includeMacroPkg);
    auto cacheIter = cacheDependencyPackageDecls.find(keyPair);
    if (cacheIter != cacheDependencyPackageDecls.cend()) {
        return cacheIter->second;
    }
    auto& packageNames = GetAllDependencyPackageNames(fullPackageName, includeMacroPkg);
    std::vector<Ptr<AST::PackageDecl>> packageDecls;
    for (auto& pkgName : packageNames) {
        auto pkgDecl = cjoManager.GetPackageDecl(pkgName);
        if (pkgDecl != nullptr) {
            packageDecls.emplace_back(pkgDecl);
        }
    }
    auto [cacheIter1, _] = cacheDependencyPackageDecls.emplace(keyPair, std::move(packageDecls));
    return cacheIter1->second;
}

const std::set<std::string>& ImportManager::DependencyGraph::GetAllDependencyPackageNames(
    const std::string& fullPackageName, bool includeMacroPkg)
{
    auto cacheIter = cacheDependencyPackageNames.find(fullPackageName);
    if (cacheIter != cacheDependencyPackageNames.cend()) {
        return cacheIter->second;
    }
    std::set<std::string> packageNames;
    // Collect all the dependencies by DFS.
    std::vector<std::string> stack;
    for (auto& edge : GetEdges(fullPackageName)) {
        stack.emplace_back(edge.first);
    }
    while (!stack.empty()) {
        auto pkgName = std::move(stack.back());
        stack.pop_back();
        if (packageNames.count(pkgName) > 0) {
            continue;
        }
        for (auto& edge : GetEdges(pkgName)) {
            stack.emplace_back(edge.first);
        }
        packageNames.emplace(std::move(pkgName));
    }
    Utils::EraseIf(packageNames, [this, includeMacroPkg](auto it) {
        return !includeMacroPkg && cjoManager.IsMacroRelatedPackageName(it);
    });
    auto [cacheIter1, _] = cacheDependencyPackageNames.emplace(fullPackageName, std::move(packageNames));
    return cacheIter1->second;
}

const std::map<std::string, std::set<Ptr<const ImportSpec>, CmpNodeByPos>>& ImportManager::DependencyGraph::GetEdges(
    const std::string& fullPackageName) const
{
    const static std::map<std::string, std::set<Ptr<const AST::ImportSpec>, AST::CmpNodeByPos>> EMPTY_EDGES;
    auto iter = dependencyMap.find(fullPackageName);
    if (iter != dependencyMap.cend()) {
        return iter->second;
    }
    return EMPTY_EDGES;
}

void ImportManager::DependencyGraph::AddDependenciesForPackage(Package& pkg)
{
    if (dependencyMap.count(pkg.fullPackageName) != 0) {
        // Ignore recursive dependency.
        return;
    }
    for (auto& file : pkg.files) {
        CJC_NULLPTR_CHECK(file);
        for (auto& import : file->imports) {
            CJC_NULLPTR_CHECK(import);
            AddDependenciesForImport(pkg, *import);
        }
    }
}

void ImportManager::DependencyGraph::AddDependenciesForImport(Package& pkg, const ImportSpec& import)
{
    auto fullPackageName = cjoManager.GetPackageNameByImport(import);
    auto package = cjoManager.GetPackage(fullPackageName);
    if (package == nullptr || package->fullPackageName == pkg.fullPackageName) {
        return; // The import may be failed. Also ignore self import.
    }
    AddDependency(pkg.fullPackageName, package->fullPackageName, import);
    AddDependenciesForPackage(*package);
    auto relation = Modules::GetPackageRelation(pkg.fullPackageName, fullPackageName);
    if (import.IsReExport() && Modules::IsVisible(import, relation)) {
        pkgReExportMap[pkg.fullPackageName].emplace(fullPackageName);
    }
}

// There is an `import v` in package `u`.
void ImportManager::DependencyGraph::AddDependency(
    const std::string& u, const std::string& v, const AST::ImportSpec& import)
{
    auto uIter = dependencyMap.find(u);
    if (uIter == dependencyMap.cend()) {
        std::set<Ptr<const AST::ImportSpec>, AST::CmpNodeByPos> imports = {&import};
        std::map<std::string, std::set<Ptr<const AST::ImportSpec>, AST::CmpNodeByPos>> edges;
        edges.emplace(v, std::move(imports));
        dependencyMap.emplace(u, std::move(edges));
    } else {
        std::map<std::string, std::set<Ptr<const AST::ImportSpec>, AST::CmpNodeByPos>>& edges = uIter->second;
        auto vIter = edges.find(v);
        if (vIter == edges.cend()) {
            std::set<Ptr<const AST::ImportSpec>, AST::CmpNodeByPos> imports = {&import};
            edges.emplace(v, std::move(imports));
        } else {
            vIter->second.emplace(&import);
        }
    }
}

void ImportManager::DependencyGraph::Clear()
{
    dependencyMap.clear();
    pkgReExportMap.clear();
    cacheDirectDependencyPackageDecls.clear();
    cacheDependencyPackageDecls.clear();
    cacheDependencyPackageNames.clear();
}
