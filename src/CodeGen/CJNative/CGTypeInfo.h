// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the utils of TypeInfo.
 */

#ifndef CANGJIE_CGTYPEINFO_H
#define CANGJIE_CGTYPEINFO_H

#include "IRBuilder.h"

namespace Cangjie::CodeGen {
namespace CGTypeInfo {
llvm::Function* GenTypeInfoFns(llvm::FunctionType* fieldFnType, CGModule& cgMod, const std::string& funcName,
    const std::unordered_map<const CHIR::GenericType*, size_t>& outerGTIdxMap, const CHIR::Type& memberType,
    const std::string& memberName);
llvm::Constant* GenSuperFnOfTypeTemplate(CGModule& cgMod, const std::string& funcName, const CHIR::Type& superType,
    const std::vector<CHIR::GenericType*>& typeArgs);
llvm::Constant* GenFieldsFnsOfTypeTemplate(
    CGModule& cgMod, const std::string& funcPrefixName, const std::vector<llvm::Constant*>& fieldsFn);
llvm::Constant* GenFieldsFnsOfTypeTemplate(CGModule& cgMod, const std::string& funcPrefixName,
    const std::vector<CHIR::Type*>& fieldTypes,
    const std::unordered_map<const CHIR::GenericType*, size_t>& genericParamIndicesMap);
} // namespace CGTypeInfo
} // namespace Cangjie::CodeGen

#endif // CANGJIE_CGTYPEINFO_H
