// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares some utility functions for suppressing warnings.
 */

#ifndef CANGJIE_UTILS_MACROS_H
#define CANGJIE_UTILS_MACROS_H

namespace Cangjie {
/// warning suppression macros
#define PRAGMA_STRINGIFY(x) #x
#ifdef __clang__
#define SUPPRESS_WARNING(w) _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wunknown-warning-option\"") \
    _Pragma(PRAGMA_STRINGIFY(clang diagnostic ignored w))
#define UNSUPPRESS_WARNING() _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define SUPPRESS_WARNING(w) _Pragma("GCC diagnostic push") \
    _Pragma(PRAGMA_STRINGIFY(GCC diagnostic ignored w))
#define UNSUPPRESS_WARNING() _Pragma("GCC diagnostic pop")
#else
#define SUPPRESS_WARNING(w)
#define UNSUPPRESS_WARNING()
#endif
}
#endif
