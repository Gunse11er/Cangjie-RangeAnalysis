// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements Semaphore class and its methods.
 */

#include "cangjie/Utils/Semaphore.h"

using namespace Cangjie::Utils;
Semaphore::Semaphore()
{
    auto numCores = std::thread::hardware_concurrency();
    // Leave 2 cores to avoid competing with user's other tasks.
    count = numCores > 2 ? numCores - 2 : 1;
}

Semaphore& Semaphore::Get()
{
    static Semaphore sem = Semaphore();
    return sem;
}

void Semaphore::Release()
{
    std::unique_lock<std::mutex> lock(mtx);
    count++;
    cv.notify_one();
}

void Semaphore::Acquire()
{
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [this] { return count > 0; });
    count--;
}

void Semaphore::SetCount(std::size_t newCount)
{
    std::unique_lock<std::mutex> lock(mtx);
    count = newCount;
}

std::size_t Semaphore::GetCount()
{
    std::unique_lock<std::mutex> lock(mtx);
    return count;
}