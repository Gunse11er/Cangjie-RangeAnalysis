// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements ICE related variables and functions.
 */

#include "cangjie/Utils/ICEUtil.h"

#include "cangjie/Basic/Version.h"
#include "cangjie/Driver/TempFileManager.h"
#include "cangjie/Frontend/CompilerInstance.h"

namespace {
using namespace Cangjie;

std::atomic<bool> g_writeOnceICEMessag(false);
} // namespace
namespace Cangjie {
namespace ICE {

int64_t TriggerPointSetter::triggerPoint = static_cast<int64_t>(Cangjie::CompileStage::COMPILE_STAGE_NUMBER);
int64_t TriggerPointSetter::interpreterTP = static_cast<int64_t>(Cangjie::CompileStage::COMPILE_STAGE_NUMBER) + 1;
int64_t TriggerPointSetter::writeCahedTP = static_cast<int64_t>(Cangjie::CompileStage::COMPILE_STAGE_NUMBER) + 2;

void TriggerPointSetter::SetICETriggerPoint(CompileStage cs)
{
    if (TriggerPointSetter::triggerPoint == Cangjie::ICE::LSP_TP) {
        return;
    }
    if (cs >= CompileStage::COMPILE_STAGE_NUMBER) {
        TriggerPointSetter::triggerPoint = static_cast<int64_t>(CompileStage::COMPILE_STAGE_NUMBER);
    } else {
        TriggerPointSetter::triggerPoint = static_cast<int64_t>(cs);
    }
}

void TriggerPointSetter::SetICETriggerPoint(int64_t tp)
{
    if (tp == Cangjie::ICE::LSP_TP) {
        TriggerPointSetter::triggerPoint = tp;
        return;
    }
    if (tp == FRONTEND_TP) {
        TriggerPointSetter::triggerPoint = static_cast<int64_t>(CompileStage::COMPILE_STAGE_NUMBER);
    } else {
        TriggerPointSetter::triggerPoint = tp;
    }
}

int64_t GetTriggerPoint()
{
    return TriggerPointSetter::triggerPoint;
}

bool CanWriteOnceICEMessage()
{
    return !g_writeOnceICEMessag.exchange(true);
}

void PrintVersionFromError()
{
    std::cerr << CANGJIE_COMPILER_VERSION << std::endl;
}

void RemoveTempFile()
{
    Cangjie::TempFileManager::Instance().DeleteTempFiles();
}

} // namespace ICE

} // namespace Cangjie
