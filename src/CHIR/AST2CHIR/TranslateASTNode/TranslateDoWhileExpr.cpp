// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/AST2CHIR/TranslateASTNode/Translator.h"

using namespace Cangjie::CHIR;
using namespace Cangjie;

Ptr<Value> Translator::Visit(const AST::DoWhileExpr& doWhileExpr)
{
    // The CHIR of DoWhileExpr is like this:
    // goto trueBlock:
    //
    // trueBlock:                    if continue, goto conditionBlock       if break, goto endBlock
    //  do-while body   -----------------------------|------------------------------|
    //  ...                                          |                              |
    //  goto conditionBlock                          |                              |
    //                                               |                              |
    // conditionBlock:                        <------|                              |
    //  %condition = ...                                                            |
    //  Branch(%condition, trueBlock, endBlock)                                     |
    //                                                                              |
    // endBlock:                              <-------------------------------------|
    // ...

    auto backupBlock = currentBlock;

    // create condition block
    Ptr<Block> conditionBlock = CreateBlock();
    // create end Block
    Ptr<Block> endBlock = CreateBlock();
    // Used for checking scope info with control flow. NOTE: location of block will not be used by CodeGen.
    // NOTE: record scope info before update the 'ScopeContext'
    auto loc = TranslateLocation(doWhileExpr);
    endBlock->SetDebugLocation(std::move(loc));
    // Set symbol table, will be used by JumpExpr.
    auto blocks = std::make_pair(conditionBlock, endBlock);
    terminatorSymbolTable.Set(doWhileExpr, blocks);

    ScopeContext context(*this);
    context.ScopePlus();
    auto const& whileLoc = TranslateLocation(*doWhileExpr.condExpr);
    // 1.create trueBlock
    TranslateSubExprToDiscarded(*doWhileExpr.body);
    Ptr<Block> trueBlock = GetBlockByAST(*doWhileExpr.body);
    // 2. goto conditionBlock.
    CreateAndAppendTerminator<GoTo>(whileLoc, conditionBlock, currentBlock)
                        ->Set<SkipCheck>(SkipKind::SKIP_DCE_WARNING);
    currentBlock = conditionBlock;
    // 3. translate condition.
    Ptr<Value> condtion = TranslateExprArg(*doWhileExpr.condExpr);
    CJC_ASSERT(
        condtion->GetType()->IsBoolean() || condtion->GetType()->IsNothing() || condtion->GetType()->IsGeneric());
    // When translate condition,may have a new block, we need to put Branch in this block.
    // example code: do {...} while (break)
    auto newConditionBlock = currentBlock;

    // go to trueBlock
    CreateAndAppendTerminator<GoTo>(trueBlock, backupBlock);

    // Do-while expr do not need to check unreachable branch.
    CreateAndAppendTerminator<Branch>(whileLoc, condtion, trueBlock, endBlock, newConditionBlock)
                          ->Set<SkipCheck>(SkipKind::SKIP_DCE_WARNING);
    currentBlock = endBlock;
    return nullptr;
}