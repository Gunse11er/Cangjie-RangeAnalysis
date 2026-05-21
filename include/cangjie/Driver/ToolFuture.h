// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares ToolFuture class and its subclasses.
 */

#ifndef CANGJIE_DRIVER_TOOLFUTURE_H
#define CANGJIE_DRIVER_TOOLFUTURE_H

#include <future>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#include <iomanip>
#undef CONST
#undef interface
#else
#include <sys/wait.h>
#endif

#include "cangjie/Utils/Utils.h"

using namespace Cangjie;

class ToolFuture {
public:
    enum class State {
        SUCCESS,
        RUNNING,
        FAILED
    };

    /**
     * @brief Get status of the asynchronous operation indicated by 'ToolFuture'.
     *
     * @return State The thread state.
     */
    virtual State GetState() = 0;

    /**
     * @brief The destructor of class ToolFuture.
     */
    virtual ~ToolFuture() {};
    std::string spawnErrorMessage;

#ifdef _WIN32
    DWORD exitCode = 0;
#else
    int exitCode = 0;
#endif
};

class ErrorFuture : public ToolFuture {
public:
    explicit ErrorFuture(const std::string& message)
    {
        spawnErrorMessage = message;
    };
    State GetState() override;
};

class ThreadFuture : public ToolFuture {
public:
    /**
     * @brief The constructor of class ThreadFuture.
     *
     * @param input The result of asynchronous operation.
     * @return ThreadFuture The thread future.
     */
    explicit ThreadFuture(std::future<bool>&& input) : future(std::move(input)) {}

    /**
     * @brief Get status of the asynchronous operation indicated by 'ThreadFuture'.
     *
     * @return State The thread state.
     */
    State GetState() override;
private:
    std::optional<bool> result = std::nullopt;
    std::future<bool> future;
};

#ifdef _WIN32
class WindowsProcessFuture : public ToolFuture {
public:
    /**
     * @brief The constructor of class WindowsProcessFuture.
     *
     * @param pi The process information.
     * @return WindowsProcessFuture The windows process future.
     */
    explicit WindowsProcessFuture(PROCESS_INFORMATION pi): pi(pi) {}

    /**
     * @brief Get status of the asynchronous operation indicated by 'WindowsProcessFuture'.
     *
     * @return State The thread state.
     */
    State GetState() override;
private:
    PROCESS_INFORMATION pi;
};
#else
class LinuxProcessFuture : public ToolFuture {
public:
    /**
     * @brief The constructor of class LinuxProcessFuture.
     *
     * @param pi The process id.
     * @return LinuxProcessFuture The linux process future.
     */
    explicit LinuxProcessFuture(pid_t pid) : pid(pid) {}

    /**
     * @brief Get status of the asynchronous operation indicated by 'LinuxProcessFuture'.
     *
     * @return State The status of asynchronous operation.
     */
    State GetState() override;
private:
    pid_t pid;
};
#endif
#endif // CANGJIE_DRIVER_TOOLFUTURE_H