// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements functions related to the Unix crash signal handler.
 */

#include "cangjie/Driver/TempFileManager.h"
#include "cangjie/Macro/InvokeUtil.h"

#if (defined RELEASE)
#include "SignalUtil.h"

namespace {
// standby stack size after stack overflow
constexpr size_t SIGNAL_STACK_SIZE = 8192;
constexpr size_t SIGNAL_NUM = 6;
// We treat following signals as crashes.
constexpr int SIGNALS[SIGNAL_NUM] = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP};
const std::vector<std::string> SIGNALS_STR = {"SIGABRT", "SIGBUS", "SIGFPE", "SIGILL", "SIGSEGV", "SIGTRAP"};

void SignalHandler(int signum, [[maybe_unused]] siginfo_t* si, [[maybe_unused]] void* arg)
{
    Cangjie::Signal::ConcurrentSynchronousSignalHandler(signum);
}
} // namespace

// Save old alternate stack.
static stack_t g_oldAltStack;
// Alternative signal stack in case stack overflow happened.
__attribute__((__used__)) static char g_altStackPointer[SIGNAL_STACK_SIZE] = {0};

namespace Cangjie {
void CreateAltSignalStack()
{
    // If there is an existing alternate signal stack, we do not introduce new one.
    if (sigaltstack(nullptr, &g_oldAltStack) != 0 || (g_oldAltStack.ss_flags & SS_ONSTACK) != 0 ||
        (g_oldAltStack.ss_sp && g_oldAltStack.ss_size >= SIGNAL_STACK_SIZE)) {
        return;
    }

    stack_t sigstack;
    sigstack.ss_sp = static_cast<void*>(g_altStackPointer);
    sigstack.ss_size = SIGNAL_STACK_SIZE;
    sigstack.ss_flags = 0;
    if (sigaltstack(&sigstack, &g_oldAltStack) != 0) {
        InternalError("Failed to create a backup stack for the thread.");
    }
}

void RegisterCrashSignalHandler()
{
    struct sigaction sa;
    sa.sa_sigaction = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    for (auto& sig : SIGNALS) {
        if (sigaction(sig, &sa, nullptr) == -1) {
            // Even if sigaction failed, we are still able to continue our compiling.
            continue;
        }
    }
}
} // namespace Cangjie

#else
#include <csignal>
#endif // (defined NDEBUG)

namespace {
void SigintHandler(int signum, [[maybe_unused]] siginfo_t* si, [[maybe_unused]] void* arg)
{
    Cangjie::TempFileManager::Instance().DeleteTempFiles();
    _exit(signum + 128);  // Add 128 to return the same error code as if the program crashed.
}
} // namespace

namespace Cangjie {
void RegisterCrtlCSignalHandler()
{
    struct sigaction sa;
    sa.sa_sigaction = SigintHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    (void)sigaction(SIGINT, &sa, nullptr);
}
} // namespace Cangjie
