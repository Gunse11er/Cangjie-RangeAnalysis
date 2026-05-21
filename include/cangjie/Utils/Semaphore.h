// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares Semaphore class.
 */

#ifndef CANGJIE_UTILS_SEMAPHORE_H
#define CANGJIE_UTILS_SEMAPHORE_H

#include <condition_variable>
#include <mutex>
#include <thread>

namespace Cangjie::Utils {

class Semaphore {
public:
    static Semaphore& Get();

    void Release();
    void Acquire();
    void SetCount(std::size_t newCount);
    std::size_t GetCount();
private:
    Semaphore();
    ~Semaphore() = default;
    Semaphore(Semaphore&&) = delete;                 // Move construct
    Semaphore(const Semaphore&) = delete;            // Copy construct
    Semaphore& operator=(const Semaphore&) = delete; // Copy assign
    Semaphore& operator=(Semaphore&&) = delete;      // Move assign

    std::mutex mtx;
    std::condition_variable cv;
    std::size_t count;
};
} // namespace Cangjie::Utils
#endif