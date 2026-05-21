// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the ToolFuture class and its subclasses.
 */

#include "cangjie/Driver/ToolFuture.h"

using namespace Cangjie;

ToolFuture::State ThreadFuture::GetState()
{
    if (!result.has_value()) {
        result = future.get();
    }
    return result.value() ? State::SUCCESS : State::FAILED;
}

ToolFuture::State ErrorFuture::GetState()
{
    return State::FAILED;
}

#ifdef _WIN32
ToolFuture::State WindowsProcessFuture::GetState()
{
    DWORD state = WaitForSingleObject(pi.hProcess, 0);
    if (state == WAIT_FAILED || state == WAIT_ABANDONED) {
        return State::FAILED;
    } else if (state == WAIT_TIMEOUT) {
        return State::RUNNING;
    }
    if (GetExitCodeProcess(pi.hProcess, &exitCode) == FALSE) {
        return State::FAILED;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0 ? State::SUCCESS : State::FAILED;
}
#else
ToolFuture::State LinuxProcessFuture::GetState()
{
    int status = 0;
    int result = waitpid(pid, &status, WNOHANG);
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    }
    if (result < 0 || status != 0) {
        // If an error occurs because the file is deleted, the error information is not printed.
        return State::FAILED;
    }
    if (result > 0) {
        return State::SUCCESS;
    }
    return State::RUNNING;
}
#endif
