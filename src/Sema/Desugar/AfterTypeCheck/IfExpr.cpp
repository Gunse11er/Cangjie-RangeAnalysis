// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Desugar/AfterTypeCheck.h"

#include "TypeCheckUtil.h"

#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Utils.h"

using namespace Cangjie;
using namespace AST;
using namespace TypeCheckUtil;
using namespace Sema::Desugar::AfterTypeCheck;

namespace {
/**
 * Insert unitExpr if the type of 'ifExpr' @p ie is unit type but the 'thenBody' is not the type of unit.
 * Also complete the 'elseBody' of the 'ifExpr' if it was not existed.
 */
void InsertUnitForIfExpr(TypeManager& tyMgr, IfExpr& ie)
{
    if (ie.desugarExpr) {
        return; // Ignore desugared expression.
    }
    // All expression after typecheck must be welltyped.
    CJC_NULLPTR_CHECK(ie.ty);
    CJC_NULLPTR_CHECK(ie.thenBody);
    // If the 'ifExpr' is not unit typed or the type of then body is the subtype of unit type, then quit process.
    auto skip = !ie.ty->IsUnit() || tyMgr.IsSubtype(ie.thenBody->ty, ie.ty);
    if (skip) {
        return;
    }
    // If the type of 'then' is not unit, then the block must not be empty.
    CJC_ASSERT(!ie.thenBody->body.empty());
    auto unitExpr = CreateUnitExpr(ie.ty);
    CopyBasicInfo(ie.thenBody->body.back().get(), unitExpr.get());
    ie.thenBody->body.push_back(std::move(unitExpr));
    ie.thenBody->ty = ie.ty;
    // If current ifExpr dose not have elseBody, create for it.
    if (!ie.elseBody) {
        // Added 'else' does not need position.
        auto elseBody = MakeOwnedNode<Block>();
        elseBody->body.push_back(CreateUnitExpr(ie.ty));
        elseBody->ty = ie.ty;
        ie.elseBody = std::move(elseBody);
        ie.hasElse = true;
        AddCurFile(*ie.elseBody, ie.curFile);
    }
}
} // namespace

namespace Cangjie::Sema::Desugar::AfterTypeCheck {
void DesugarIfExpr(TypeManager& typeManager, IfExpr& ifExpr)
{
    InsertUnitForIfExpr(typeManager, ifExpr);
}
} // namespace Cangjie::Sema::Desugar::AfterTypeCheck
