// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_ANNOINFO_H
#define CANGJIE_CHIR_ANNOINFO_H

#include <string>

#include "cangjie/CHIR/IR/DebugLocation.h"

/** @Annotation
 *  class Anno1 {
 *      const init(param1: Type, param2: Type, ...)
 *  }
 *  @Annotation
 *  class Anno2 {
 *      const init(param1: Type, param2: Type, ...)
 *  }
 *  @Anno1[arg1, arg2, ...]
 *  @Anno2[arg3, arg4, ...]
 *  class CA {}
 *  as we can see, @Anno1 and @Anno2 are `AnnoInfo` of class CA, we create annotation factory function for runtime,
 *  we only need to store its mangled name
 *  as for `@Anno1[arg1, arg2, ...]` and `@Anno2[arg3, arg4, ...]`, we call them custom annotation instance,
 *  usually, we only need to store its class name and arguments
 */
namespace Cangjie::CHIR {
class CustomAnnoInstance {
public:
    CustomAnnoInstance(
        const std::string& className, const std::vector<std::string>& argValues, const DebugLocation& loc);
    std::string ToString(size_t indent) const;
    void Dump() const;
    std::string GetAnnoClassName() const;
    const std::vector<std::string>& GetArgValues() const;
    const DebugLocation& GetDebugLocation() const;

private:
    std::string annoClassName;
    std::vector<std::string> argValues;
    DebugLocation loc;
};

class AnnoInfo {
public:
    AnnoInfo();
    AnnoInfo(const std::string& funcName, std::vector<CustomAnnoInstance>&& instances);
    bool IsAvailable() const;
    std::string ToString(size_t indent) const;
    void Dump() const;
    std::string GetAnnoFactoryFuncMangledName() const;
    const std::vector<CustomAnnoInstance>& GetCustomAnnoInstances() const;

private:
    // attention: we have a deal with llvm ir, we use "none" to stand there is no custom annotation
    // of course, we can use other word or just empty string, you just remember to modify it in llvm-project, too
    std::string mangledName{"none"};
    std::vector<CustomAnnoInstance> annoInstances;
};
} // namespace Cangjie::CHIR

#endif
