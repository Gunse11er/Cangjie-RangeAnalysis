// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Diags.h"

#include "TypeCheckUtil.h"
#include "cangjie/Basic/DiagnosticEngine.h"

using namespace Cangjie;
using namespace AST;

namespace Cangjie::Sema {
void DiagInvalidMultipleAssignExpr(
    DiagnosticEngine& diag, const Node& leftNode, const Expr& rightExpr, const std::string& because)
{
    auto builder = diag.DiagnoseRefactor(DiagKindRefactor::sema_mismatched_types_multiple_assign, rightExpr);
    builder.AddMainHintArguments(rightExpr.GetTy()->String());
    builder.AddHint(leftNode, because);
}

void DiagInvalidBinaryExpr(DiagnosticEngine& diag, const BinaryExpr& be)
{
    CJC_NULLPTR_CHECK(be.leftExpr);
    CJC_NULLPTR_CHECK(be.rightExpr);
    CJC_ASSERT(Ty::IsTyCorrect(be.leftExpr->GetTy()));
    CJC_ASSERT(Ty::IsTyCorrect(be.rightExpr->GetTy()));
    const std::string& opStr = TOKENS[static_cast<int>(be.op)];
    auto range = be.operatorPos.IsZero() ? MakeRange(be.begin, be.end) : MakeRange(be.operatorPos, opStr);
    auto builder = diag.DiagnoseRefactor(DiagKindRefactor::sema_invalid_binary_expr, be, range, opStr,
        be.leftExpr->GetTy()->String(), be.rightExpr->GetTy()->String());
    if (be.leftExpr->GetTy()->IsFunc() || be.leftExpr->GetTy()->IsCFunc() || be.leftExpr->GetTy()->IsTuple()) {
        // func and tuple type cannot be extended
        return;
    }
    if (TypeCheckUtil::IsOverloadableOperator(be.op)) {
        std::string note("you may want to implement 'operator func " + opStr +
            "(right: " + be.rightExpr->GetTy()->String() + ")' for type '" + be.leftExpr->GetTy()->String() + "'");
        if (be.op == TokenKind::EXP) {
            if (be.leftExpr->TyKind() == TypeKind::TYPE_INT64) {
                note += ", or to provide a right operand of type 'UInt64'";
            } else if (be.leftExpr->TyKind() == TypeKind::TYPE_FLOAT64) {
                note += ", or to provide a right operand of type 'Int64' or 'Float64'";
            } else if (be.rightExpr->TyKind() == TypeKind::TYPE_INT64) {
                note += ", or to provide a left operand of type 'Float64'";
            } else if (be.rightExpr->TyKind() == TypeKind::TYPE_FLOAT64) {
                note += ", or to provide a left operand of type 'Float64'";
            } else if (be.rightExpr->TyKind() == TypeKind::TYPE_UINT64) {
                note += ", or to provide a left operand of type 'Int64'";
            }
        }
        builder.AddNote(note);
    }
}

void DiagInvalidUnaryExpr(DiagnosticEngine& diag, const UnaryExpr& ue)
{
    if (!ue.ShouldDiagnose()) {
        return;
    }
    CJC_NULLPTR_CHECK(ue.expr);
    CJC_ASSERT(Ty::IsTyCorrect(ue.expr->GetTy()));
    const std::string& opStr = TOKENS[static_cast<int>(ue.op)];
    auto builder =
        diag.DiagnoseRefactor(DiagKindRefactor::sema_invalid_unary_expr, ue, opStr, ue.expr->GetTy()->String());
    if (ue.expr->GetTy()->IsExtendable()) {
        builder.AddNote(
            "you may want to implement 'operator func " + opStr + "()' for type '" + ue.expr->GetTy()->String() + "'");
    }
}

void DiagInvalidUnaryExprWithTarget(DiagnosticEngine& diag, const UnaryExpr& ue, Ty& target)
{
    if (!ue.ShouldDiagnose()) {
        return;
    }
    CJC_NULLPTR_CHECK(ue.expr);
    CJC_ASSERT(Ty::IsTyCorrect(ue.expr->GetTy()));
    CJC_ASSERT(Ty::IsTyCorrect(&target));
    const std::string& opStr = TOKENS[static_cast<int>(ue.op)];
    auto builder = diag.DiagnoseRefactor(
        DiagKindRefactor::sema_invalid_unary_expr_with_target, ue, opStr, ue.expr->GetTy()->String(), target.String());
}

void DiagInvalidSubscriptExpr(
    DiagnosticEngine& diag, const SubscriptExpr& se, const Ty& baseTy, const std::vector<Ptr<Ty>>& indexTys)
{
    if (!se.ShouldDiagnose()) {
        return;
    }
    CJC_ASSERT(!indexTys.empty());
    CJC_ASSERT(Ty::IsTyCorrect(&baseTy));
    CJC_ASSERT(Ty::AreTysCorrect(indexTys));
    std::string indexPrefix = "type" + (indexTys.size() > 1 ? std::string("s ") : " ");
    std::string indexStr = indexPrefix + "'" + Ty::GetTypesToStr(indexTys, "', ") + "'";
    auto builder = diag.DiagnoseRefactor(DiagKindRefactor::sema_invalid_subscript_expr, se, baseTy.String(), indexStr);
    if (baseTy.IsExtendable()) {
        std::string indexParam;
        for (size_t i = 0; i < indexTys.size(); ++i) {
            indexParam += "index" + std::to_string(i) + ": " + indexTys[i]->String();
            if (i != indexTys.size() - 1) {
                indexParam += ", ";
            }
        }
        builder.AddNote(
            "you may want to implement 'operator func [](" + indexParam + ")' for type '" + baseTy.String() + "'");
    }
}

void DiagUnableToInferExpr(DiagnosticEngine& diag, const Expr& expr)
{
    (void)diag.DiagnoseRefactor(DiagKindRefactor::sema_unable_to_infer_expr, expr);
}
} // namespace Cangjie::Sema
