// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Test cases for type alias parser.
 */
#include "gtest/gtest.h"

#include "cangjie/Parse/Parser.h"

using namespace Cangjie;
using namespace AST;

TEST(ParseTypeAlias, ParseTypeAliasTest)
{
    {
        std::string code = R"(
        type* d = dd
        )";
        SourceManager sm;
        DiagnosticEngine diag;
        diag.SetSourceManager(&sm);
        Parser p(code, diag, sm);
        OwnedPtr<Node> node = p.ParseDecl(ScopeKind::TOPLEVEL);
        diag.EmitCategoryDiagnostics(DiagCategory::PARSE);
        EXPECT_TRUE(diag.GetErrorCount() > 0);
    }
}
