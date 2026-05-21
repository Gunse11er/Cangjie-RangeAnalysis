// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements crash signal handler related functions.
 */

#if (defined RELEASE)
#include "SignalUtil.h"

#include <cstring>
#include "cangjie/Basic/Version.h"
#include "cangjie/Driver/TempFileManager.h"

namespace {
#ifdef CANGJIE_BUILD_TESTS
// The file handle of output to stderror.
// In the case of a signal test, it could be a real file handle.
int g_errorFd = STDERR_FILENO;
// void (*)(int) test callback function pointer
Cangjie::SignalTest::SignalTestCallbackFuncType g_signalTestCallbackFunction = nullptr;
// The test callback function is not executed by default.
Cangjie::SignalTest::TriggerPointer g_signalTestCallbackFunctionTriggerPoint =
    Cangjie::SignalTest::TriggerPointer::NON_POINTER;
#else
// The file handle of output to stderror
constexpr int g_errorFd = STDERR_FILENO;
#endif

std::atomic<bool> g_processingSignalOrException(false);
constexpr size_t LOOP_SIZE = 100000;

void AsyncSigSafeReverse(char str[])
{
    for (size_t i = 0, j = strlen(str) - 1; i < j; i++, j--) {
        char c = str[i];
        str[i] = str[j];
        str[j] = c;
    }
}

/* Convert int to base 10 string (from K&R) */
void AsyncSigSafeItoa(int64_t num, char str[])
{
    bool neg = num < 0;
    if (neg) {
        num = -num;
    }
    int64_t c = 0;
    size_t i = 0;
    const int64_t base = 10; // decimal
    do {
        c = num % base;
        str[i++] =
            static_cast<char>((c < base) ? (c + static_cast<int64_t>('0')) : ((c - base) + static_cast<int64_t>('a')));
    } while ((num /= base) > 0);
    if (neg) {
        str[i++] = '-';
    }
    str[i] = '\0';
    AsyncSigSafeReverse(str);
}

ssize_t AsyncSigSafeWriteToError(const char str[])
{
    return write(g_errorFd, str, strlen(str));
}

ssize_t AsyncSigSafePut(int64_t num) /* Put int */
{
    char str[128] = {0};
    AsyncSigSafeItoa(num, str); /* Based on K&R itoa() */
    return AsyncSigSafeWriteToError(str);
}

#ifdef CANGJIE_BUILD_TESTS
void CloseTempFileHandle()
{
    if (g_errorFd != STDERR_FILENO) {
        close(g_errorFd);
        g_errorFd = STDERR_FILENO;
    }
}
#endif
} // namespace

using namespace Cangjie;

void Signal::WriteICEMessage(int64_t errorCode)
{
    (void)AsyncSigSafeWriteToError(Cangjie::CANGJIE_COMPILER_VERSION.c_str());
    (void)AsyncSigSafeWriteToError("\n");
    (void)AsyncSigSafeWriteToError(Cangjie::ICE::MSG_PART_ONE.c_str());
    (void)AsyncSigSafeWriteToError(Cangjie::SIGNAL_MSG_PART_ONE.c_str());
    (void)AsyncSigSafePut(errorCode);
    (void)AsyncSigSafeWriteToError(Cangjie::SIGNAL_MSG_PART_TWO.c_str());
    (void)AsyncSigSafeWriteToError(Cangjie::ICE::MSG_PART_TWO.c_str());
    (void)AsyncSigSafePut(Cangjie::ICE::GetTriggerPoint());
    (void)AsyncSigSafeWriteToError("\n");
#ifdef CANGJIE_BUILD_TESTS
    CloseTempFileHandle();
#endif
}

void Signal::ThreadDelaySynchronizer()
{
    // When multiple threads call this function at the same time, only the first thread can exit immediately,
    // and other threads can exit at a later time.
    if (g_processingSignalOrException.exchange(true)) {
        for (size_t i = LOOP_SIZE; i > 0; --i) {
            asm volatile(""); // This assembly prevents the loop from being optimized.
        }
    }
}

void Signal::ConcurrentSynchronousSignalHandler(int signum)
{
    ThreadDelaySynchronizer();
    WriteICEMessage(signum);
    Cangjie::TempFileManager::Instance().DeleteTempFilesSignalSafe();
    int exitCode = 128 + signum; // Add 128 to return the same error code as if the program crashed.
    _exit(exitCode);
}

#ifdef CANGJIE_BUILD_TESTS

void SignalTest::SetSignalTestCallbackFunc(SignalTestCallbackFuncType fp, TriggerPointer triggerPoint, int fd)
{
    g_signalTestCallbackFunction = fp;
    g_signalTestCallbackFunctionTriggerPoint = triggerPoint;
    g_errorFd = fd;
}

void SignalTest::ExecuteSignalTestCallbackFunc(TriggerPointer executionPoint)
{
    if (g_signalTestCallbackFunction == nullptr) {
        return;
    }
    if (executionPoint == TriggerPointer::NON_POINTER ||
        g_signalTestCallbackFunctionTriggerPoint == TriggerPointer::NON_POINTER) {
        return;
    }
    if (executionPoint != g_signalTestCallbackFunctionTriggerPoint) {
        return;
    }
    g_signalTestCallbackFunction();
}

#endif // CANGJIE_BUILD_TESTS

#endif // (defined NDEBUG)