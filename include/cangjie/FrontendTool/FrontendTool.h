// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares frontend execute apis.
 */

#ifndef CANGJIE_FRONTENDTOOL_FRONTENDTOOL_H
#define CANGJIE_FRONTENDTOOL_FRONTENDTOOL_H

#include <string>
#include <vector>

#include "cangjie/Frontend/FrontendObserver.h"

namespace Cangjie {
class Driver;
/**
 * Execute frontend.
 */
int ExecuteFrontend(const std::string& exePath, const std::vector<std::string>& args,
    const std::unordered_map<std::string, std::string>& environmentVars);

/**
 * Execute frontend by compiler invocation.
 */
bool ExecuteFrontendByDriver(DefaultCompilerInstance& instance, const Driver& driver);

bool NeedCreateIncrementalCompilerInstance(const GlobalOptions& opts);
} // namespace Cangjie
#endif // CANGJIE_FRONTENDTOOL_FRONTENDTOOL_H
