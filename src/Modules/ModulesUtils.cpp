// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements utilities for modules.
 */

#include "cangjie/Modules/ModulesUtils.h"

namespace Cangjie::Modules {
PackageRelation GetPackageRelation(const std::string& srcFullPkgName, const std::string& targetFullPkgName)
{
    auto pureSrcFullPackageName = ImportManager::IsTestPackage(srcFullPkgName)
        ? ImportManager::GetMainPartPkgNameForTestPkg(srcFullPkgName)
        : srcFullPkgName;
    auto pureTargetFullPackageName = ImportManager::IsTestPackage(targetFullPkgName)
        ? ImportManager::GetMainPartPkgNameForTestPkg(targetFullPkgName)
        : targetFullPkgName;
    if (pureSrcFullPackageName == pureTargetFullPackageName) {
        return PackageRelation::SAME_PACKAGE;
    }
    if (pureSrcFullPackageName == "" || pureTargetFullPackageName == "") {
        return PackageRelation::NONE;
    }

    auto srcPath = Utils::SplitQualifiedName(pureSrcFullPackageName);
    auto targetPath = Utils::SplitQualifiedName(pureTargetFullPackageName);
    if (targetPath.size() < srcPath.size() &&
        std::equal(targetPath.begin(), targetPath.end(), srcPath.begin(),
            srcPath.begin() + static_cast<ptrdiff_t>(targetPath.size()))) {
        return PackageRelation::CHILD;
    }

    return srcPath.front() == targetPath.front() ? PackageRelation::SAME_MODULE : PackageRelation::NONE;
}
} // namespace Cangjie::Modules
