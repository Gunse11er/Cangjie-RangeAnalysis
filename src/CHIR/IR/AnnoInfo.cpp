// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/IR/AnnoInfo.h"
#include "cangjie/CHIR/Utils/ToStringUtils.h"

#include <iostream>
#include <sstream>

using namespace Cangjie::CHIR;

CustomAnnoInstance::CustomAnnoInstance(
    const std::string& className, const std::vector<std::string>& argValues, const DebugLocation& loc)
    : annoClassName(className), argValues(argValues), loc(loc)
{
}

std::string CustomAnnoInstance::ToString(size_t indent) const
{
    // @className[arg1, arg2, ...] // loc
    std::stringstream ss;
    ss << IndentToString(indent) << "@" << annoClassName;
    if (auto argStr = StringJoin(argValues, ", "); !argStr.empty()) {
        ss << "[" << argStr << "]";
    }
    ss << CommentToString(loc.ToString());
    return ss.str();
}

void CustomAnnoInstance::Dump() const
{
    std::cout << ToString(0) << std::endl;
}

std::string CustomAnnoInstance::GetAnnoClassName() const
{
    return annoClassName;
}

const std::vector<std::string>& CustomAnnoInstance::GetArgValues() const
{
    return argValues;
}

const DebugLocation& CustomAnnoInstance::GetDebugLocation() const
{
    return loc;
}

AnnoInfo::AnnoInfo()
{
}

AnnoInfo::AnnoInfo(const std::string& funcName, std::vector<CustomAnnoInstance>&& instances)
    : mangledName(funcName), annoInstances(std::move(instances))
{
}

bool AnnoInfo::IsAvailable() const
{
    return mangledName != "none";
}

std::string AnnoInfo::ToString(size_t indent) const
{
    // @AnnoFactoryFunc: mangled name
    // @class1Name[arg1, arg2, ...] // loc
    // @class2Name[arg1, arg2, ...] // loc
    if (!IsAvailable()) {
        return "";
    }
    std::stringstream ss;
    ss << IndentToString(indent) << "@AnnoFactoryFunc: " << mangledName;
    for (const auto& anno : annoInstances) {
        ss << std::endl << anno.ToString(indent);
    }
    return ss.str();
}

std::string AnnoInfo::GetAnnoFactoryFuncMangledName() const
{
    return mangledName;
}

const std::vector<CustomAnnoInstance>& AnnoInfo::GetCustomAnnoInstances() const
{
    return annoInstances;
}