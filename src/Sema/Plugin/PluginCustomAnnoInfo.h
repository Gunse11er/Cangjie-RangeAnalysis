// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares lightweight APILevel annotation metadata structures.
 */

#ifndef PLUGIN_CUSTOM_ANNO_INFO_H
#define PLUGIN_CUSTOM_ANNO_INFO_H

#include <optional>
#include <string>

#include "APILevelVersion.h"

namespace Cangjie {
namespace PluginCheck {

using APILevelVersion = Cangjie::APILevelVersion;

struct PluginCustomAnnoInfo {
    APILevelVersion since;
    std::string syscap{""};
    std::optional<bool> hasHideAnno{std::nullopt};
};

} // namespace PluginCheck
} // namespace Cangjie

#endif
