// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements diagnostic suppressor class for semantic check.
 */

#include "DiagSuppressor.h"

using namespace Cangjie;
using namespace std;

std::vector<Diagnostic> DiagSuppressor::GetSuppressedDiag()
{
    return diag.ConsumeStoredDiags();
}

void DiagSuppressor::ReportDiag()
{
    auto diags = GetSuppressedDiag();
    diag.EnableDiagnose(originDiagVec);
    for (auto& d : diags) {
        diag.Diagnose(d);
    }
    originDiagVec = diag.DisableDiagnose();
}

bool DiagSuppressor::HasError() const
{
    return Utils::In(diag.GetStoredDiags(),
        [](const Diagnostic& d) { return d.diagSeverity == DiagSeverity::DS_ERROR; });
}
