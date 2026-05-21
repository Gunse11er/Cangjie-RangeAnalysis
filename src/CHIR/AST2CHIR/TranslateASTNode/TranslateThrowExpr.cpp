// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/AST2CHIR/TranslateASTNode/Translator.h"

using namespace Cangjie::CHIR;
using namespace Cangjie;

Ptr<Value> Translator::Visit(const AST::ThrowExpr& throwExpr)
{
    auto loc = TranslateLocation(throwExpr.begin, throwExpr.end);
    CJC_NULLPTR_CHECK(throwExpr.expr);
    auto eVal = TranslateExprArg(*throwExpr.expr);
    Ptr<Terminator> terminator = nullptr;
    if (tryCatchContext.empty()) {
        terminator = CreateAndAppendTerminator<RaiseException>(loc, eVal, currentBlock);
    } else {
        auto errBB = tryCatchContext.top();
        terminator = CreateAndAppendTerminator<RaiseException>(loc, eVal, errBB, currentBlock);
    }
    // For following unreachable expressions, and throw also has value of type 'Nothing'.
    currentBlock = CreateBlock();
    maybeUnreachable.emplace(currentBlock, terminator);
    return nullptr;
}
