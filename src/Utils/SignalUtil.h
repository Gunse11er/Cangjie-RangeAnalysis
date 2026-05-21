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

#ifndef CANGJIE_UTILS_SIGNALUTIL_H
#define CANGJIE_UTILS_SIGNALUTIL_H

#if (defined RELEASE)

#include <atomic>
#include <memory>
#include <unistd.h>
#include "cangjie/Utils/Signal.h"

namespace Cangjie {
namespace Signal {
void WriteICEMessage(int64_t errorCode);
void ThreadDelaySynchronizer();
void ConcurrentSynchronousSignalHandler(int signum);
} // namespace Signal
} // namespace Cangjie

#endif

#endif // CANGJIE_UTILS_SIGNALUTIL_H