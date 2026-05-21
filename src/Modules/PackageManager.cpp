// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements PackageManager related classes.
 */

#include "cangjie/Modules/PackageManager.h"

using namespace Cangjie;
using namespace AST;

void PackageManager::CollectDepsInFile(File& file, const std::unique_ptr<PackageInfo>& pkgInfo, const Package& pkg)
{
    for (auto& it : file.imports) {
        std::string fullPkgName = importManager.cjoManager->GetPackageNameByImport(*it);
        auto pd = importManager.cjoManager->GetPackageDecl(fullPkgName);
        if (pd == nullptr) {
            continue;
        }
        CJC_ASSERT(pd && pd->srcPackage);
        auto importPkg = pd->srcPackage;
        // Global init func should not be generated for Java package.
        if (importPkg->TestAttr(AST::Attribute::TOOL_ADD)) {
            continue;
        }
        fullPkgName = importPkg->fullPackageName;
        auto found = packageInfoMap.find(fullPkgName);
        if (found != packageInfoMap.end()) {
            pkgInfo->deps.insert(found->second.get());
        } else {
            (void)packageToOtherSccMap[pkg.fullPackageName].emplace(importPkg->fullPackageName);
        }
    }
}

void PackageManager::CollectDeps(std::vector<Ptr<Package>>& pkgs)
{
    for (auto& pkg : pkgs) {
        auto [_, success] =
            packageInfoMap.emplace(pkg->fullPackageName, std::make_unique<PackageInfo>(pkg->fullPackageName));
        CJC_ASSERT(success);
    }
    for (auto& pkg : pkgs) {
        auto& pkgInfo = packageInfoMap[pkg->fullPackageName];
        for (auto& file : pkg->files) {
            CollectDepsInFile(*file, pkgInfo, *pkg);
        }
    }
}

// Using Tarjan Algorithm to resolve circular dependence between two or more packages
// and get their buildOrders.
// The function invoke TarjanForSCC and use "for loop" resolve the condition having isolated island.
bool PackageManager::ResolveDependence(std::vector<Ptr<Package>>& pkgs)
{
    packageInfoMap.clear();
    orderedPackageInfos.clear();
    packageToOtherSccMap.clear();
    CollectDeps(pkgs);
    TarjanContext ctx = {};
    std::stack<PackageInfo*> st;
    size_t index = 0;
    while (ctx.indices.size() < packageInfoMap.size()) {
        if (ctx.indices.empty()) {
            TarjanForSCC(ctx, st, index, packageInfoMap.begin()->second.get());
            continue;
        }
        for (auto& i : packageInfoMap) {
            if (ctx.indices[i.second.get()] == 0) {
                TarjanForSCC(ctx, st, index, i.second.get());
            }
        }
    }
    std::unordered_map<std::string, Ptr<Package>> sortPackageMap;
    for (auto& pkg : pkgs) {
        sortPackageMap[pkg->fullPackageName] = pkg;
    }
    bool ret = true;
    size_t pkgIndex = 0;
    for (auto& it : orderedPackageInfos) {
        // Currently compiler does not support to compile multiple circular dependent source packages.
        // CjoGen will resolve circular dependency before this process.
        ret = ret && it.size() == 1;
        for (auto& pkg : it) {
            pkgs[pkgIndex++] = sortPackageMap[pkg->fullPackageName];
        }
    }
    return ret;
}

void PackageManager::TarjanForSCC(TarjanContext& ctx, std::stack<PackageInfo*>& st, size_t& index, PackageInfo* u)
{
    // Set the bookkeeping info for `u` to the smallest unused `index`.
    ++index;
    ctx.indices[u] = index;
    ctx.lowlinks[u] = index;
    st.emplace(u);
    ctx.onStack[u] = true;

    PackageInfo* v = nullptr;
    for (auto w : u->deps) {
        v = w;
        if (ctx.indices[v] == 0) {
            TarjanForSCC(ctx, st, index, v);
            ctx.lowlinks[u] = std::min(ctx.lowlinks[u], ctx.lowlinks[v]);
        } else if (ctx.onStack[v]) {
            ctx.lowlinks[u] = std::min(ctx.lowlinks[u], ctx.indices[v]);
        }
    }
    if (ctx.lowlinks[u] == ctx.indices[u]) {
        std::vector<PackageInfo*> infos;
        do {
            v = st.top();
            infos.emplace_back(v);
            st.pop();
            ctx.onStack[v] = false;
        } while (v != u);
        orderedPackageInfos.emplace_back(infos);
    }
}
