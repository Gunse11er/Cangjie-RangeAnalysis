// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Driver, who runs the compiler.
 */

#ifndef CANGJIE_DRIVER_DRIVER_H
#define CANGJIE_DRIVER_DRIVER_H

#include <memory>
#include <string>
#include <vector>

#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/Driver/DriverOptions.h"

namespace Cangjie {
class Driver {
public:
    /**
     * @brief The constructor of class Driver.
     *
     * @param args The arguments vector.
     * @param diag The main diagnostic processing center.
     * @param exeName The name of exe.
     * @return Driver The instance of Driver.
     */
    Driver(const std::vector<std::string>& args, DiagnosticEngine& diag, const std::string& exeName = "cjc");

    /**
     * @brief Parse arguments and setup options specified by user in the command.
     *
     * @return bool Return true If success.
     */
    bool ParseArgs();

    /**
     * @brief Read necessary paths from environment variables and store them in GlobalOptions.
     *
     * @param environmentVars The environment variables.
     */
    void EnvironmentSetup(const std::unordered_map<std::string, std::string>& environmentVars);

    /**
     * @brief The main function of the compilation process.
     *
     * @return bool Return true If success.
     */
    bool ExecuteCompilation() const;

    /**
     * @brief Generate backend and linking commands.
     *
     * @return bool Return true If success.
     */
    bool InvokeCompileToolchain() const;

    std::vector<std::string> args;
    DiagnosticEngine& diag;
    std::unique_ptr<OptionTable> optionTable;
    std::unique_ptr<ArgList> argList;
    std::unique_ptr<DriverOptions> driverOptions;
    std::string executableName;
    std::string cangjieHome;
};
} // namespace Cangjie

#endif // CANGJIE_DRIVER_DRIVER_H
