// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the Backend related classes.
 */

#include "cangjie/Driver/Backend/Backend.h"

#include "cangjie/Basic/Print.h"
#include "Job.h"

namespace {
// List of extensions of output files which should not be customized.
const std::string AST_EXT = "cjo";    // *.ast file, otherwise it will shadow the real ast file.

using namespace Cangjie;

// Check file name for final output.
bool CheckOutputNameByTrustList(const std::string& file)
{
    auto ext = FileUtil::GetFileExtension(file);
    if (ext == AST_EXT) {
        return false;
    }

    return true;
}
}; // namespace

bool Backend::Generate()
{
    if (!GenerateToolChain() || !TC) {
        return false;
    }
    TC->InitializeLibraryPaths();
    if (!PrepareDependencyPath()) {
        return false;
    }
    // Check invalid suffix of output name for different backends
    if (!FileUtil::IsDir(driverOptions.output) &&
        !CheckOutputNameByTrustList(driverOptions.output)) {
        Errorf("file extension '.%s' is not allowed, please change it\n",
            FileUtil::GetFileExtension(driverOptions.output).c_str());
        return false;
    }
    return ProcessGeneration();
}