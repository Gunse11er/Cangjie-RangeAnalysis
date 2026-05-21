// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Backend related classes, which provides versatile compile utility.
 */

#ifndef CANGJIE_DRIVER_BACKEND_H
#define CANGJIE_DRIVER_BACKEND_H

#include "cangjie/Driver/DriverOptions.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"
#include "cangjie/Option/Option.h"

namespace Cangjie {
class Job;

/**
 * Backend is an interface. An implementation of Backend generates one or more
 * tools. A tool contains a tool name (e.g. llc, javac), some arguments (e.g. -O2, -static, -L.), and
 * some necessary environment settings for generating an actual executable command. A tool represents
 * a well-structured command. Backend itself does not execute any commands.
 */
class Backend {
public:
    /**
     * @brief The constructor of class Backend.
     *
     * @param job The compilation job.
     * @param driverOptions The data structure is obtained through parsing the compilation options.
     * @param driver It is the object that triggers the compiler's compilation process.
     * @return Backend The instance of Backend.
     */
    explicit Backend(Job& job, const DriverOptions& driverOptions, const Driver& driver)
        : driver(driver), driverOptions(driverOptions), ownerJob(job)  {}

    /**
     * @brief The destructor of class Backend.
     */
    virtual ~Backend() = default;

    /**
     * @brief Generate toolchain, assembly tools.
     *
     * @return bool Return true If generate success.
     */
    bool Generate();

    const std::vector<ToolBatch>& GetBackendCmds()
    {
        return backendCmds;
    }

protected:
    const Driver& driver;
    const DriverOptions& driverOptions;
    std::unique_ptr<ToolChain> TC;
    Job& ownerJob;
    std::vector<ToolBatch> backendCmds;

    virtual bool GenerateToolChain() = 0;
    virtual bool ProcessGeneration() = 0;

    /**
     * Check whether tools exist (and get their paths if required)
     * If some dependencies are not available, this function should return false so the generation won't proceed.
     */
    virtual bool PrepareDependencyPath() = 0;
};
} // namespace Cangjie
#endif // CANGJIE_DRIVER_BACKEND_H
