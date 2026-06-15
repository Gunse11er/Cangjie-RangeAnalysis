// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the Desugar function.
 */
#ifndef CANGJIE_SEMA_DESUGAR_H
#define CANGJIE_SEMA_DESUGAR_H

#include <string>

#include "cangjie/AST/Node.h"

namespace Cangjie {
using namespace AST;
/// Run all desugar passes over @p root before type-checking.
/// @param root               The AST root node to walk.
/// @param desugarMacrocall   When true, also desugar macro-call nodes from the LSP
///                           originalMacroCallNodes list (i.e. enableMacroInLSP mode).
/// @param compatibleSDKVersion  The value of --cfg=APILevel_level (e.g. "26.1.5").
///                           Controls which IfAvailable desugar path is taken (see
///                           DesugarIfAvailableLevelCondition). Empty string means no
///                           compatible SDK version is configured.
/// Note: desugarMacrocall and compatibleSDKVersion are kept as separate parameters
/// because they are semantically orthogonal: one governs LSP macro expansion, the
/// other governs runtime API-level gating. Merging them into a struct would obscure
/// this distinction.
void PerformDesugarBeforeTypeCheck(
    Node& root, bool desugarMacrocall = false, const std::string& compatibleSDKVersion = "");
} // namespace Cangjie

#endif
