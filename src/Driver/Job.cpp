// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the Job Class.
 */

#include "Job.h"

#include "cangjie/Basic/Print.h"
#include "cangjie/Driver/Tool.h"
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
#include "cangjie/Driver/Backend/CJNATIVEBackend.h"
#endif
#include "cangjie/Driver/TempFileManager.h"
#include "cangjie/Driver/Toolchains/ToolChain.h"
#include "cangjie/Utils/FileUtil.h"
#include "cangjie/Utils/ProfileRecorder.h"
#include "cangjie/Utils/Semaphore.h"
#ifdef _WIN32
#include "cangjie/Basic/StringConvertor.h"
#endif

namespace {
bool CheckExecuteResult(std::map<std::string, std::unique_ptr<ToolFuture>>& checklist,
    bool returnIfAnyToolFinished = false)
{
    auto printError = [](const std::string& cmd, const std::unique_ptr<ToolFuture>& future) {
#ifdef _WIN32
        std::optional<std::wstring> werrCmd = Cangjie::StringConvertor::StringToWString(cmd);
        if (werrCmd.has_value()) {
            WErrorf(L"%ls: command failed with exit code %lu (use -V to see invocation)\n", werrCmd.value().c_str(),
                future.get()->exitCode);
        }
#else
        Errorf(
            "%s: command failed with exit code %d (use -V to see invocation)\n", cmd.c_str(), future.get()->exitCode);
#endif
    };
    bool success = true;
    while (!checklist.empty()) {
        size_t totalTasks = checklist.size();
        for (auto it = checklist.cbegin(); it != checklist.cend();) {
            auto state = it->second->GetState();
            if (state == ToolFuture::State::FAILED) {
                Utils::Semaphore::Get().Release();
                printError(it->first, it->second);
                checklist.erase(it++);
                success = false;
            } else if (state == ToolFuture::State::SUCCESS) {
                Utils::Semaphore::Get().Release();
                checklist.erase(it++);
            } else {
                ++it;
            }
        }
        if (returnIfAnyToolFinished && totalTasks != checklist.size()) {
            // Some tasks are finished and removed from the list.
            return success;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200)); // Check running tasks every 200 ms.
    }
    return success;
}
} // namespace

using namespace Cangjie;

bool Job::Assemble(const DriverOptions& driverOptions, const Driver& driver)
{
    switch (driverOptions.backend) {
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
        case Triple::BackendType::CJNATIVE:
            backend = std::make_unique<CJNATIVEBackend>(*this, driverOptions, driver);
            break;
#endif
        case Triple::BackendType::UNKNOWN:
        default:
            Errorln("Toolchain: Unsupported backend");
            return false;
    }

    if (!backend->Generate()) {
        return false;
    }

    verbose = driverOptions.enableVerbose;

    return true;
}

bool Job::Execute() const
{
    const std::vector<ToolBatch>& commandList = backend->GetBackendCmds();
    for (const ToolBatch& cmdBatch : commandList) {
        if (cmdBatch.empty()) {
            continue;
        }
        std::map<std::string, std::unique_ptr<ToolFuture>> childWorkers{};
        Utils::ProfileRecorder recorder("Main Stage", "Execute " + FileUtil::GetFileName(cmdBatch[0]->GetName()), "");
        for (auto& cmd : cmdBatch) {
            // NOTE: `CheckExecuteResult` acquires semaphore without condition. We must ensure that there is still
            // available slot in semaphore before executing next command. If there is no more slot available, wait
            // any of previous created threads finish.
            while (Utils::Semaphore::Get().GetCount() == 0) {
                if (!CheckExecuteResult(childWorkers, true)) {
                    return false;
                }
            }
            auto future = cmd->Execute(verbose);
            if (!future || !future->spawnErrorMessage.empty()) {
#ifdef _WIN32
                std::optional<std::wstring> wcmdString =
                    Cangjie::StringConvertor::StringToWString(cmd->GetCommandString());
                std::optional<std::wstring> werrString =
                    Cangjie::StringConvertor::StringToWString(future->spawnErrorMessage);
                if (wcmdString.has_value() && werrString.has_value() && !werrString.value().empty()) {
                    WErrorf(L"%ls: command failed (%ls)\n", wcmdString.value().c_str(), werrString.value().c_str());
                } else if (wcmdString.has_value()) {
                    WErrorf(L"%ls: command failed (use -V to see invocation)\n", wcmdString.value().c_str());
                }
#else
                std::string commandString = cmd->GetCommandString();
                if (!future->spawnErrorMessage.empty()) {
                    Errorf("%s: command failed (%s)\n", commandString.c_str(), future->spawnErrorMessage.c_str());
                } else {
                    Errorf("%s: command failed (use -V to see invocation)\n", commandString.c_str());
                }
#endif
                return false;
            }
            childWorkers.emplace(cmd->GetCommandString(), std::move(future));
        }
        if (!CheckExecuteResult(childWorkers)) {
            return false;
        }
    }
    return true;
}