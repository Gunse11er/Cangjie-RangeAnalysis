// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements functions related to the Windows crash signal handler.
 */

#include "cangjie/Driver/TempFileManager.h"

#if (defined RELEASE)
#include "SignalUtil.h"

namespace {
LONG WINAPI WindowsExceptionHandler(LPEXCEPTION_POINTERS ep)
{
    Cangjie::Signal::ThreadDelaySynchronizer();
    // Write out the exception code.
    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    int64_t exitCode = ep->ExceptionRecord->ExceptionCode;
    Cangjie::Signal::WriteICEMessage(exitCode);
    Cangjie::TempFileManager::Instance().DeleteTempFiles();
    return exitCode;
}

void SignalHandler(int signum)
{
    Cangjie::Signal::ConcurrentSynchronousSignalHandler(signum);
}

} // namespace

namespace Cangjie {
void RegisterCrashExceptionHandler()
{
    SetUnhandledExceptionFilter(WindowsExceptionHandler); // Windows API
}

void RegisterCrashSignalHandler()
{
    int signals[] = {SIGABRT, SIGFPE, SIGILL, SIGSEGV};
    for (auto& sig : signals) {
        if (signal(sig, SignalHandler) == SIG_ERR) {
            // Even if sigaction failed, we are still able to continue our compiling.
            continue;
        }
    }
}
} // namespace Cangjie
#else
#include <csignal>
#include <windows.h>
#endif // (defined NDEBUG)

namespace {
BOOL WINAPI LLVMConsoleCtrlHandler(DWORD dwCtrlType)
{
    Cangjie::TempFileManager::Instance().DeleteTempFiles();
    return FALSE;
}
} // namespace

namespace Cangjie {
void RegisterCrtlCSignalHandler()
{
    (void)SetConsoleCtrlHandler(LLVMConsoleCtrlHandler, TRUE);
}
} // namespace Cangjie
