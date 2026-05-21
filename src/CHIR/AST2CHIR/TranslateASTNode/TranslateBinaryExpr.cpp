// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/AST2CHIR/TranslateASTNode/Translator.h"

using namespace Cangjie::CHIR;
using namespace Cangjie;

Ptr<Value> Translator::Visit(const AST::BinaryExpr& binaryExpr)
{
    if (!binaryExpr.TestAttr(AST::Attribute::SIDE_EFFECT)) {
        return ProcessBinaryExpr(binaryExpr);
    }
    /* The following cangjie code may have side effect, and sema will set attribute SIDE_EFFECT on binaryExpr.
        test(1) == (true, true, true))
        desugar to:
        test(1)[0] == true && test(1)[1] == true && test(1)[2] == true
                                                | |=========================> has the Attribute: SIDE_EFFECT
        |     | ============================================================> the callExpr has mapExpr.

    */
    if (auto rightBinaryExpr = DynamicCast<AST::BinaryExpr*>(binaryExpr.rightExpr.get()); rightBinaryExpr) {
        AST::SubscriptExpr* subscriptExpr = nullptr;
        if (rightBinaryExpr->leftExpr && rightBinaryExpr->leftExpr->astKind == AST::ASTKind::SUBSCRIPT_EXPR) {
            subscriptExpr = StaticCast<AST::SubscriptExpr*>(rightBinaryExpr->leftExpr.get());
        } else if (rightBinaryExpr->rightExpr && rightBinaryExpr->rightExpr->astKind == AST::ASTKind::SUBSCRIPT_EXPR) {
            subscriptExpr = StaticCast<AST::SubscriptExpr*>(rightBinaryExpr->rightExpr.get());
        }
        if (subscriptExpr) {
            if (auto mapExpr = GetMapExpr(*subscriptExpr->baseExpr)) {
                if (!exprValueTable.Has(*mapExpr)) {
                    auto chirNode = TranslateExprArg(*subscriptExpr->baseExpr);
                    exprValueTable.Set(*mapExpr, *chirNode);
                }
            }
        }
    }
    return ProcessBinaryExpr(binaryExpr);
}

Ptr<Value> Translator::ProcessBinaryExpr(const AST::BinaryExpr& binaryExpr)
{
    // BinaryExpression init func is (ExprKind, Value*, Value*, OverflowStrategy, Block*)
    static const std::unordered_map<Cangjie::TokenKind, ExprKind> OP2_EXPR_KIND = {
        {Cangjie::TokenKind::ADD, ExprKind::ADD},
        {Cangjie::TokenKind::SUB, ExprKind::SUB},
        {Cangjie::TokenKind::MUL, ExprKind::MUL},
        {Cangjie::TokenKind::DIV, ExprKind::DIV},
        {Cangjie::TokenKind::MOD, ExprKind::MOD},
        {Cangjie::TokenKind::EXP, ExprKind::EXP},
        {Cangjie::TokenKind::AND, ExprKind::AND},
        {Cangjie::TokenKind::OR, ExprKind::OR},
        {Cangjie::TokenKind::BITAND, ExprKind::BITAND},
        {Cangjie::TokenKind::BITOR, ExprKind::BITOR},
        {Cangjie::TokenKind::BITXOR, ExprKind::BITXOR},
        {Cangjie::TokenKind::LSHIFT, ExprKind::LSHIFT},
        {Cangjie::TokenKind::RSHIFT, ExprKind::RSHIFT},
        {Cangjie::TokenKind::LT, ExprKind::LT},
        {Cangjie::TokenKind::GT, ExprKind::GT},
        {Cangjie::TokenKind::LE, ExprKind::LE},
        {Cangjie::TokenKind::GE, ExprKind::GE},
        {Cangjie::TokenKind::NOTEQ, ExprKind::NOTEQUAL},
        {Cangjie::TokenKind::EQUAL, ExprKind::EQUAL},
    };
    const auto chirType = TranslateType(*binaryExpr.ty);
    const auto& loc = TranslateLocation(binaryExpr);
    auto it = OP2_EXPR_KIND.find(binaryExpr.op);
    CJC_ASSERT(it != OP2_EXPR_KIND.end());
    ExprKind kd = it->second;
    auto lhs = TranslateExprArg(*binaryExpr.leftExpr);
    CJC_NULLPTR_CHECK(lhs);
    if (kd == ExprKind::AND) {
        return TransShortCircuitAnd(lhs, *binaryExpr.rightExpr, loc, binaryExpr.TestAttr(AST::Attribute::COMPILER_ADD));
    }
    if (kd == ExprKind::OR) {
        return TransShortCircuitOr(lhs, *binaryExpr.rightExpr, loc, binaryExpr.TestAttr(AST::Attribute::COMPILER_ADD));
    }
    auto rightExpr = TranslateExprArg(*binaryExpr.rightExpr);
    bool mayHaveException = OverloadableExprMayThrowException(binaryExpr, *chirType);
    if (binaryExpr.leftExpr->ty->IsNothing()) {
        const auto& rightExprLoc = TranslateLocation(*binaryExpr.rightExpr);
        return TryCreateWithOV<BinaryExpression>(currentBlock, mayHaveException, binaryExpr.overflowStrategy,
            rightExprLoc, loc, chirType, kd, lhs, rightExpr)
            ->GetResult();
    } else {
        const auto& operatorLoc = GetOperatorLoc(binaryExpr);
        return TryCreateWithOV<BinaryExpression>(
            currentBlock, mayHaveException, binaryExpr.overflowStrategy, operatorLoc, loc, chirType, kd, lhs, rightExpr)
            ->GetResult();
    }
}

Ptr<Value> Translator::TransShortCircuitAnd(
    const Ptr<Value> leftValue, const AST::Expr& rightExpr, const DebugLocation& loc, bool isImplicitAdd)
{
    auto expr = CreateAndAppendExpression<Allocate>(
        loc, builder.GetType<RefType>(builder.GetBoolTy()), builder.GetBoolTy(), currentBlock);
    Ptr<Value> alloca = expr->GetResult();
    Ptr<Block> thenBlock = CreateBlock();
    Ptr<Block> elseBlock = CreateBlock();
    Ptr<Block> endBlock = CreateBlock();
    CJC_ASSERT(
        leftValue->GetType()->IsBoolean() || leftValue->GetType()->IsNothing() || leftValue->GetType()->IsGeneric());
    const auto& rightLoc = TranslateLocation(rightExpr);
    CreateWrappedBranch(SourceExpr::BINARY, loc, leftValue, thenBlock, elseBlock, currentBlock);
    // Create Then Block:
    currentBlock = thenBlock;
    if (!isImplicitAdd) {
        currentBlock->SetDebugLocation(rightLoc);
    }
    TranslateSubExprToLoc(rightExpr, alloca, rightLoc);
    CreateAndAppendTerminator<GoTo>(loc, endBlock, currentBlock);
    // Create Else Block:
    auto boolLiteral =
        CreateAndAppendConstantExpression<BoolLiteral>(loc, builder.GetBoolTy(), *elseBlock, false)->GetResult();
    CreateWrappedStore(loc, boolLiteral, alloca, elseBlock);
    CreateAndAppendTerminator<GoTo>(loc, endBlock, elseBlock);
    // Update 'currentBlock' at last.
    currentBlock = endBlock;
    return CreateAndAppendExpression<Load>(loc, builder.GetBoolTy(), alloca, endBlock)->GetResult();
}

Ptr<Value> Translator::TransShortCircuitOr(
    const Ptr<Value> leftValue, const AST::Expr& rightExpr, const DebugLocation& loc, bool isImplicitAdd)
{
    Ptr<Value> alloca = CreateAndAppendExpression<Allocate>(
        loc, builder.GetType<RefType>(builder.GetBoolTy()), builder.GetBoolTy(), currentBlock)
                            ->GetResult();
    Ptr<Block> thenBlock = CreateBlock();
    Ptr<Block> elseBlock = CreateBlock();
    Ptr<Block> endBlock = CreateBlock();
    CJC_ASSERT(
        leftValue->GetType()->IsBoolean() || leftValue->GetType()->IsNothing() || leftValue->GetType()->IsGeneric());
    const auto& rightLoc = TranslateLocation(rightExpr);
    CreateWrappedBranch(SourceExpr::BINARY, loc, leftValue, thenBlock, elseBlock, currentBlock);
    // Create Then Block:
    auto boolLiteral =
        CreateAndAppendConstantExpression<BoolLiteral>(builder.GetBoolTy(), *thenBlock, true)->GetResult();
    CreateWrappedStore(rightLoc, boolLiteral, alloca, thenBlock);
    CreateAndAppendTerminator<GoTo>(rightLoc, endBlock, thenBlock);
    // Create Else Block:
    currentBlock = elseBlock;
    if (!isImplicitAdd) {
        currentBlock->SetDebugLocation(rightLoc);
    }
    TranslateSubExprToLoc(rightExpr, alloca, rightLoc);
    CreateAndAppendTerminator<GoTo>(rightLoc, endBlock, currentBlock);
    // Update 'currentBlock' at last.
    this->currentBlock = endBlock;
    return CreateAndAppendExpression<Load>(loc, builder.GetBoolTy(), alloca, endBlock)->GetResult();
}
