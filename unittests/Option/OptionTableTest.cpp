// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <memory>

#include "gtest/gtest.h"

#include "cangjie/Basic/Print.h"
#include "cangjie/Option/OptionTable.h"
#include "cangjie/Utils/FileUtil.h"

using namespace Cangjie;

class OptionTableTest : public ::testing::Test {
protected:
    void SetUp() override
    {
    }

    std::unique_ptr<OptionTable> table = CreateOptionTable();
};

// Special case: invalid input
TEST_F(OptionTableTest, ParseArgsTestEmptyInput)
{
    ArgList argList;
    std::vector<std::string> argStrs;
    (void)table->ParseArgs(argStrs, argList);
    // We don't really care about the result in such cases, we just don't want it crashes.
}

// Correct case: empty arg
TEST_F(OptionTableTest, ParseArgsTestEmptyArg)
{
    // Special case: Empty string
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", ""};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);
}

// Correct case: space only arg
TEST_F(OptionTableTest, ParseArgsTestSpaceArg)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", " "};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);
}

// Error case: dash only arg
TEST_F(OptionTableTest, ParseArgsTestDashOnlyArg)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_FALSE(succ);
}

// Error case: Separated
TEST_F(OptionTableTest, ParseArgsTestSeparatedError)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-o"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_FALSE(succ);
}

// Correct case: Separated
TEST_F(OptionTableTest, ParseArgsTestSeparatedCorrect)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-o", "a.out"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);
}

// Error case: Flag
TEST_F(OptionTableTest, ParseArgsTestFlagError)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-not-exist-arg"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_FALSE(succ);
}

// Correct case: Flag
TEST_F(OptionTableTest, ParseArgsTestFlagCorrect)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-v"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);
}

// Error case: Joined, wrong option name
TEST_F(OptionTableTest, ParseArgsTestJoinedError)
{
    ArgList argList5;
    std::vector<std::string> argStrs5 = {"cjc", "-not-exist-arg=local"};
    bool succ = table->ParseArgs(argStrs5, argList5);
    EXPECT_FALSE(succ);
}

// Correct case: Joined
TEST_F(OptionTableTest, ParseArgsTestJoinedCorrect)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "--output-type=staticlib"};
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);
}

// Correct case: multi correct arguments
TEST_F(OptionTableTest, ParseArgsTestCorrect1)
{
    std::vector<std::string> argStrs = {"cjc-frontend", "--dump-ir", "main.cj", "test.cj"};
    ArgList argList;
    std::unique_ptr<OptionTable> table = CreateOptionTable(true);
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);

    EXPECT_EQ(argList.args.size(), 3);
    auto arg = dynamic_cast<OptionArgInstance*>(argList.args[0].get());
    EXPECT_NE(arg, nullptr);
    EXPECT_EQ(arg->info.GetID(), Options::ID::DUMP_IR);

    auto input1 = dynamic_cast<InputArgInstance*>(argList.args[1].get());
    auto input2 = dynamic_cast<InputArgInstance*>(argList.args[2].get());
    EXPECT_EQ(input1->value, "main.cj");
    EXPECT_EQ(input2->value, "test.cj");
}

// Correct case: have separated kind arguments in the arguments string
TEST_F(OptionTableTest, ParseArgsTestCorrect2)
{
    std::vector<std::string> argStrs = {"cjc-frontend", "-Woff", "all", "main.cj"};
    ArgList argList;
    std::unique_ptr<OptionTable> table = CreateOptionTable(true);
    bool succ = table->ParseArgs(argStrs, argList);
    EXPECT_TRUE(succ);

    EXPECT_EQ(argList.args.size(), 2);
    auto arg = dynamic_cast<OptionArgInstance*>(argList.args[0].get());
    EXPECT_NE(arg, nullptr);
    EXPECT_EQ(arg->info.GetID(), Options::ID::WARN_OFF);
    EXPECT_EQ(arg->value, "all");
}

TEST_F(OptionTableTest, ArgListTest)
{
    ArgList argList;
    std::vector<std::string> argStrs = {"cjc", "-Woff", "all", "main.cj"};
    table->ParseArgs(argStrs, argList);

    EXPECT_EQ(argList.args.size(), 2);
    auto arg = dynamic_cast<OptionArgInstance*>(argList.args[0].get());
    EXPECT_NE(arg, nullptr);
    EXPECT_EQ(arg->info.GetID(), Options::ID::WARN_OFF);
    EXPECT_EQ(arg->value, "all");
}

// FLAG options must be set MULTIPLE_OCCURRENCE for occurrence type. See Options.inc for details.
TEST_F(OptionTableTest, FlagOccurrenceSetTest)
{
    std::unique_ptr<OptionTable> optionTable = CreateOptionTable();
    const auto& optionInfos = optionTable->optionInfos;
    for (OptionTable::OptionInfo info : optionInfos) {
        if (info.GetKind() == Options::Kind::FLAG) {
            EXPECT_EQ(info.GetOccurrenceType(), Options::Occurrence::MULTIPLE_OCCURRENCE);
        }
    }
}

/**
 * READ THE FOLLOWING NOTE if you found this test failed.
 *
 * What is this?
 * This is a unittest tests if every SEPARATED kind options with SINGLE_OCCURRENCE property has a
 * corresponding LLT test in a particular LLT directory.
 *
 * What is the purpose of this test?
 * When an option (SEPARATED kind options with SINGLE_OCCURRENCE property) is specified in a
 * command more than once, an warning will be printed to remind user the former specified one in
 * the command will be overwritten by the later specified one. We need to add PROPER tests for this
 * warning print feature.
 *
 * What should I do?
 * Add a test in the particular LLT directory. You should check the following items MANUALLY:
 *   - if the warning makes sense (does you option overwrite former specified value?)
 *   - if conflict warnings have been printed (warnings and errors that contradict)
 * The test itself could be simply check the output of warning. However, the above checks
 * needs to be done by the developer.
 *
 * Note: The test file should have a proper name for passing this unittest.
 */
TEST_F(OptionTableTest, DISABLED_OccurrenceTestExistTest)
{
#ifdef PROJECT_SOURCE_DIR
    std::string projectPath = PROJECT_SOURCE_DIR;
#else
    std::string projectPath = "..";
#endif
    std::string testsPath = projectPath + "/tests/LLT/Driver/options/occurance_warning_tests";
    std::unique_ptr<OptionTable> optionTable = CreateOptionTable();
    const auto& optionInfos = optionTable->optionInfos;
    for (OptionTable::OptionInfo info : optionInfos) {
        if (info.GetOccurrenceType() == Options::Occurrence::SINGLE_OCCURRENCE) {
            if (info.GetName() == "--module-name") {
                continue;
            }
            std::string fileName = info.GetName() + ".cj";
            std::string fullPath = FileUtil::JoinPath(testsPath, fileName);
            EXPECT_TRUE(FileUtil::FileExist(fullPath)) << fullPath << " not exist.";
        }
    }
}
