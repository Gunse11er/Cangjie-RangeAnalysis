// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares ICE related variables and functions.
 */

#ifndef CANGJIE_UTILS_ICE_H
#define CANGJIE_UTILS_ICE_H

#include "cangjie/Basic/Color.h"
#include "cangjie/Utils/CheckUtils.h"

#include <string>
#include <unistd.h>

namespace Cangjie {
enum class CompileStage;
namespace ICE {
const int EXIT_CODE = 2; // Internal compiler error
const std::string MSG_PART_ONE = ANSI_COLOR_RED + "Internal Compiler Error: " + ANSI_COLOR_RESET;
const std::string MSG_PART_TWO = "\nPlease report this to Cangjie team and include the project. Error Code: ";
constexpr int64_t FRONTEND_TP = -1;
constexpr int64_t UNITTEST_TP = -2;
constexpr int64_t LSP_TP = -3;
void PrintVersionFromError();
void RemoveTempFile();
int64_t GetTriggerPoint();

class TriggerPointSetter {
public:
    TriggerPointSetter(CompileStage cs)
    {
        SetICETriggerPoint(cs);
    }

    TriggerPointSetter(int64_t tp)
    {
        SetICETriggerPoint(tp);
    }

    ~TriggerPointSetter()
    {
        SetICETriggerPoint();
    }

    friend int64_t GetTriggerPoint();

    static int64_t interpreterTP;  // Module code of the interpreter stage, which is equal to frontend code plus 1.
    static int64_t writeCahedTP;  // Module code of the write cahed stage, which is equal to frontend code plus 2.

private:
    // Save global ICE trigger point
    static int64_t triggerPoint;
    static void SetICETriggerPoint(CompileStage cs);
    static void SetICETriggerPoint(int64_t tp = ICE::FRONTEND_TP);
};

bool CanWriteOnceICEMessage();
} // namespace ICE

template <typename... Args> inline void InternalError(Args&&... args)
{
    if (ICE::CanWriteOnceICEMessage()) {
        ICE::PrintVersionFromError();
        std::cerr << ICE::MSG_PART_ONE;
        ((std::cerr << args), ...);
        int64_t tp = ICE::GetTriggerPoint();
        std::cerr << ICE::MSG_PART_TWO << std::to_string(tp) << std::endl;
        // When ut and lsp cases are compiled, do not exit after ICE,
        // because some ut cases are designed to go to the wrong branch.
        if (tp == ICE::LSP_TP || tp == ICE::UNITTEST_TP) {
            return;
        }
        ICE::RemoveTempFile();

#ifdef NDEBUG
        _exit(ICE::EXIT_CODE);
#else
        CJC_ASSERT(false);
#endif
    }
}

template <typename... Args> inline void InternalError(bool pred, Args&&... args)
{
    if (!pred) {
        InternalError(std::forward<Args>(args)...);
    }
}

} // namespace Cangjie

#endif // CANGJIE_UTILS_ICE_H
