// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Test cases for incremental parse for LSP.
 */
#include "gtest/gtest.h"
#include <functional>
#include <unordered_map>

#include "TestCompilerInstance.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/AST/Match.h"
#include "cangjie/Parse/Parser.h"
#include "cangjie/Utils/CastingTemplate.h"

using namespace Cangjie;
using namespace AST;

class ParseIncrTest : public testing::Test {
protected:
    void SetUp() override
    {
#ifdef __x86_64__
        invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::X86_64;
#else
        invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::AARCH64;
#endif
#ifdef _WIN32
        invocation.globalOptions.target.os = Cangjie::Triple::OSType::WINDOWS;
        invocation.globalOptions.executablePath = projectPath + "\\output\\bin\\";
#elif __unix__
        invocation.globalOptions.target.os = Cangjie::Triple::OSType::LINUX;
        invocation.globalOptions.executablePath = projectPath + "/output/bin/";
#endif
        invocation.globalOptions.importPaths = {definePath};
    }

#ifdef PROJECT_SOURCE_DIR
    // Gets the absolute path of the project from the compile parameter.
    std::string projectPath = PROJECT_SOURCE_DIR;
#else
    // Just in case, give it a default value.
    // Assume the initial is in the build directory.
    std::string projectPath = "..";
#endif
    std::string srcPath;
    std::string definePath;
    DiagnosticEngine diag;
    SourceManager sm;
    std::string codeOfFile1 = R"(
    package test
    public class Test1 {
    }
    )";
    std::string newCodeOfFile1 = R"(
    package test
    public class NewTest1 {
    }
    )";
    std::string codeOfFile2 = R"(
    package test
    public class Test2 {
    }
    )";
    CompilerInvocation invocation;
    std::unique_ptr<TestCompilerInstance> instance;
};

void ResetCount(size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    macthTest1Count = 0;
    macthTest2Count = 0;
    macthNewTest1Count = 0;
}

using CountSymbolsFunc = std::function<VisitAction(Ptr<Node>)>;

CountSymbolsFunc CreateCountSymbolsLambda(size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    return [&macthTest1Count, &macthTest2Count, &macthNewTest1Count](Ptr<Node> curNode) -> VisitAction {
        if (curNode->astKind == ASTKind::CLASS_DECL) {
            const auto& decl = *StaticAs<ASTKind::CLASS_DECL>(curNode);
            if (decl.identifier.Val() == "Test1") {
                macthTest1Count++;
            } else if (decl.identifier.Val() == "Test2") {
                macthTest2Count++;
            } else if (decl.identifier.Val() == "NewTest1") {
                macthNewTest1Count++;
            }
            return VisitAction::SKIP_CHILDREN;
        }
        return VisitAction::WALK_CHILDREN;
    };
}

void WalkAndVerifyCounts(TestCompilerInstance& instance, const CountSymbolsFunc& countSymbols,
                         size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count,
                         size_t expectedTest1, size_t expectedTest2, size_t expectedNewTest1)
{
    Walker testWalker(instance.GetSourcePackages()[0], countSymbols);
    testWalker.Walk();
    EXPECT_EQ(macthTest1Count, expectedTest1);
    EXPECT_EQ(macthTest2Count, expectedTest2);
    EXPECT_EQ(macthNewTest1Count, expectedNewTest1);
}

bool SetBufferCacheAndParse(TestCompilerInstance& instance,
                            const std::unordered_map<std::string, CompilerInstance::SrcCodeCacheInfo>& cache)
{
    instance.bufferCache = cache;
    return instance.PerformParse();
}

void TestInitialParse(TestCompilerInstance& instance, const CountSymbolsFunc& countSymbols,
                      size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    bool parseRes = instance.PerformParse();
    EXPECT_TRUE(parseRes);
    WalkAndVerifyCounts(instance, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count, 1, 1, 0);
}

void TestChangedParse(TestCompilerInstance& instance, const std::string& newCodeOfFile1,
                      const std::string& codeOfFile2, const CountSymbolsFunc& countSymbols,
                      size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    std::unordered_map<std::string, CompilerInstance::SrcCodeCacheInfo> cache = {
        {"File1.cj", {CompilerInstance::SrcCodeChangeState::CHANGED, newCodeOfFile1}},
        {"File2.cj", {CompilerInstance::SrcCodeChangeState::UNCHANGED, codeOfFile2}}
    };
    bool parseRes = SetBufferCacheAndParse(instance, cache);
    EXPECT_TRUE(parseRes);
    ResetCount(macthTest1Count, macthTest2Count, macthNewTest1Count);
    WalkAndVerifyCounts(instance, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count, 0, 1, 1);
}

void TestDeletedParse(TestCompilerInstance& instance, const std::string& codeOfFile2,
                      const CountSymbolsFunc& countSymbols,
                      size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    std::unordered_map<std::string, CompilerInstance::SrcCodeCacheInfo> cache = {
        {"File1.cj", {CompilerInstance::SrcCodeChangeState::DELETED, ""}},
        {"File2.cj", {CompilerInstance::SrcCodeChangeState::UNCHANGED, codeOfFile2}}
    };
    bool parseRes = SetBufferCacheAndParse(instance, cache);
    EXPECT_TRUE(parseRes);
    ResetCount(macthTest1Count, macthTest2Count, macthNewTest1Count);
    WalkAndVerifyCounts(instance, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count, 0, 1, 0);
}

void TestReAddedParse(TestCompilerInstance& instance, const std::string& codeOfFile1,
                      const std::string& codeOfFile2, const CountSymbolsFunc& countSymbols,
                      size_t& macthTest1Count, size_t& macthTest2Count, size_t& macthNewTest1Count)
{
    std::unordered_map<std::string, CompilerInstance::SrcCodeCacheInfo> cache = {
        {"File1.cj", {CompilerInstance::SrcCodeChangeState::ADDED, codeOfFile1}},
        {"File2.cj", {CompilerInstance::SrcCodeChangeState::UNCHANGED, codeOfFile2}}
    };
    bool parseRes = SetBufferCacheAndParse(instance, cache);
    EXPECT_TRUE(parseRes);
    ResetCount(macthTest1Count, macthTest2Count, macthNewTest1Count);
    WalkAndVerifyCounts(instance, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count, 1, 1, 0);
}

TEST_F(ParseIncrTest, ParseIncrTest)
{
    instance = std::make_unique<TestCompilerInstance>(invocation, diag);
    instance->bufferCache = {
        {"File1.cj", {CompilerInstance::SrcCodeChangeState::ADDED, codeOfFile1}},
        {"File2.cj", {CompilerInstance::SrcCodeChangeState::ADDED, codeOfFile2}}
    };
    invocation.globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
    instance->loadSrcFilesFromCache = true;
    size_t macthTest1Count{0};
    size_t macthTest2Count{0};
    size_t macthNewTest1Count{0};
    auto countSymbols = CreateCountSymbolsLambda(macthTest1Count, macthTest2Count, macthNewTest1Count);
    TestInitialParse(*instance, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count);
    TestChangedParse(*instance, newCodeOfFile1, codeOfFile2, countSymbols,
                     macthTest1Count, macthTest2Count, macthNewTest1Count);
    TestDeletedParse(*instance, codeOfFile2, countSymbols, macthTest1Count, macthTest2Count, macthNewTest1Count);
    TestReAddedParse(*instance, codeOfFile1, codeOfFile2, countSymbols,
                     macthTest1Count, macthTest2Count, macthNewTest1Count);
}
