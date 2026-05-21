// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Driver's Job class.
 */

#ifndef CANGJIE_DRIVER_JOB_H
#define CANGJIE_DRIVER_JOB_H

#include <string>
#include <vector>

#include "cangjie/Driver/Backend/Backend.h"
#include "cangjie/Driver/DriverOptions.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"
#include "cangjie/Option/Option.h"

namespace Cangjie {
class Job {
public:
    Job()
    {
    }
    ~Job() = default;

    /**
     * Assembly the job and toolchain command.
     */
    bool Assemble(const DriverOptions& driverOptions, const Driver& driver);

    /**
     * Execute compilation job.
     */
    bool Execute() const;

private:
    std::unique_ptr<Backend> backend;
    std::vector<std::string> tmpFiles;
    bool verbose{false};
};
} // namespace Cangjie

#endif // CANGJIE_DRIVER_JOB_H
