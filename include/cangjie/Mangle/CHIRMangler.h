// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the CHIRMangle.
 */

#ifndef CANGJIE_MANGLE_CHIRMANGLER_H
#define CANGJIE_MANGLE_CHIRMANGLER_H

#include "cangjie/Mangle/BaseMangler.h"

namespace Cangjie::CHIR {
/**
 * Name mangle class
 * Class CHIRMangler is designed to manage mangling rules during CHIR compilation.
 *
 */
class CHIRMangler : public BaseMangler {
public:
    /**
     * @brief The constructor of class CHIRMangler.
     *
     * @param compileTest The variable to enable compile test.
     * @return CHIRMangler The instance of CHIRMangler.
     */
    CHIRMangler(bool compileTest) : BaseMangler(), enableCompileTest(compileTest){};

    /**
     * @brief The constructor of class CHIRMangler.
     *
     * @param delimiter The delimiter of module.
     * @param compileTest The variable to enable compile test.
     * @return CHIRMangler The instance of CHIRMangler.
     */
    CHIRMangler(const std::string& delimiter, bool compileTest)
        : BaseMangler(delimiter), enableCompileTest(compileTest){};

    /**
     * @brief The destructor of class CHIRMangler.
     */
    virtual ~CHIRMangler() = default;

protected:
    /**
     * @brief Mangle the signature of CFunc.
     *        eg: the signature of CFunc<(Int64, Bool)->Float64> is "dlb".
     *
     * @param cFuncTy Indicates it is the sema type.
     * @return std::string The mangled signature.
     */
    std::string MangleCFuncSignature(const AST::FuncTy& cFuncTy) const;

private:
    std::optional<std::string> MangleEntryFunction(const Cangjie::AST::FuncDecl& funcDecl) const override;

    bool enableCompileTest;
};

} // namespace Cangjie::CHIR
#endif // CANGJIE_MANGLE_CHIRMANGLER_H
