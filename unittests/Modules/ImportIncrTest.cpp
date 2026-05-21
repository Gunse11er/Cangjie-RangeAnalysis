// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Test cases for incremental import for LSP.
 */
#include "gtest/gtest.h"

#include "TestCompilerInstance.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Parse/Parser.h"
#include "cangjie/Utils/CastingTemplate.h"

using namespace Cangjie;
using namespace AST;

class ImportIncrTest : public testing::Test {
protected:
    void SetUp() override
    {
        SetupTempDirectory();
        SetupCompilerOptions();
        CompileCoreMock();
        CompileAllFiles();
    }

    void SetupTempDirectory()
    {
#ifdef _WIN32
        std::string command;
        int err = 0;
        if (FileUtil::FileExist("testTempFiles")) {
            std::string command = "rmdir /s /q testTempFiles";
            int err = system(command.c_str());
            std::cout << err;
        }
        command = "mkdir testTempFiles";
        err = system(command.c_str());
        ASSERT_EQ(0, err);
        srcPath = projectPath + "\\unittests\\Modules\\CangjieFiles\\";
        packagePath = packagePath + "\\";
#else
        std::string command;
        int err = 0;
        if (FileUtil::FileExist("testTempFiles")) {
            command = "rm -rf testTempFiles";
            err = system(command.c_str());
            ASSERT_EQ(0, err);
        }
        command = "mkdir -p testTempFiles";
        err = system(command.c_str());
        ASSERT_EQ(0, err);
        srcPath = projectPath + "/unittests/Modules/CangjieFiles/";
        packagePath = packagePath + "/";
#endif
    }

    void SetupCompilerOptions()
    {
#ifdef __x86_64__
        invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::X86_64;
#else
        invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::AARCH64;
#endif
#ifdef _WIN32
        invocation.globalOptions.target.os = Cangjie::Triple::OSType::WINDOWS;
#elif __unix__
        invocation.globalOptions.target.os = Cangjie::Triple::OSType::LINUX;
#endif
        invocation.globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
        invocation.globalOptions.disableReflection = true;
        invocation.globalOptions.importPaths = {packagePath};
        invocation.globalOptions.compilationCachedPath = ".";
    }

    void CompileCoreMock()
    {
        auto coreFile = srcPath + "coremock.cj";
        std::string failedReason;
        auto content = FileUtil::ReadFileContent(coreFile, failedReason);
        if (!content.has_value()) {
            diag.DiagnoseRefactor(
                DiagKindRefactor::module_read_file_to_buffer_failed, DEFAULT_POSITION, coreFile, failedReason);
        }
        instance = std::make_unique<TestCompilerInstance>(invocation, diag);
        CreateModulesDirectory();
        instance->code = std::move(content.value());
        instance->invocation.globalOptions.implicitPrelude = false;
        instance->Compile(CompileStage::DESUGAR_AFTER_SEMA);
        std::vector<uint8_t> astData;
        instance->importManager.ExportAST(false, astData, *instance->GetSourcePackages()[0]);
        allAstData["std.core"] = astData;
        instance->invocation.globalOptions.implicitPrelude = true; // reset to default
    }

    void CreateModulesDirectory()
    {
        auto modulesName = FileUtil::JoinPath(instance->cangjieHome, "modules");
        auto libPathName = invocation.globalOptions.GetCangjieLibTargetPathName();
        auto cangjieModules = FileUtil::JoinPath(modulesName, libPathName);
        if (!FileUtil::FileExist(cangjieModules)) {
            FileUtil::CreateDirs(FileUtil::JoinPath(cangjieModules, ""));
        }
    }

    void CompileAllFiles()
    {
        for (unsigned int i = 0; i < fileNames.size(); i++) {
            CompileSingleFile(fileNames[i]);
        }
    }

    void CompileSingleFile(const std::string& fileName)
    {
        auto srcFile = srcPath + fileName + ".cj";
        std::string failedReason;
        auto content = FileUtil::ReadFileContent(srcFile, failedReason);
        if (!content.has_value()) {
            diag.DiagnoseRefactor(
                DiagKindRefactor::module_read_file_to_buffer_failed, DEFAULT_POSITION, srcFile, failedReason);
        }
        instance = std::make_unique<TestCompilerInstance>(invocation, diag);
        instance->importManager.SetPackageCjoCache("std.core", allAstData["std.core"]);
        instance->code = std::move(content.value());
        instance->Compile();
        std::vector<uint8_t> astData;
        instance->importManager.ExportAST(false, astData, *instance->GetSourcePackages()[0]);
        allAstData[fileName] = astData;
        diag.Reset();
    }
#ifdef PROJECT_SOURCE_DIR
    // Gets the absolute path of the project from the compile parameter.
    std::string projectPath = PROJECT_SOURCE_DIR;
#else
    // Just in case, give it a default value.
    // Assume the initial is in the build directory.
    std::string projectPath = "..";
#endif
    std::vector<std::string> fileNames{
        "vardecl", "funcdecl", "math", "recorddecl", "interfacedecl", "classdecl", "extenddecl"};
    std::string packagePath = "testTempFiles";
    std::string srcPath;
    DiagnosticEngine diag;
    CompilerInvocation invocation;
    std::unique_ptr<TestCompilerInstance> instance;
    std::unordered_map<std::string, std::vector<uint8_t>> allAstData;
};

namespace {
bool FindDeclInPackage(
    const std::vector<std::pair<std::string, std::vector<Ptr<Decl>>>>& decls, const std::string& packageName)
{
    for (auto& [name, decls] : decls) {
        for (auto& decl : decls) {
            if (decl->fullPackageName == packageName) {
                return true;
            }
        }
    }
    return false;
}

void SetupPackageCaches(TestCompilerInstance& instance,
                        const std::unordered_map<std::string, std::vector<uint8_t>>& allAstData,
                        const std::vector<std::string>& fileNames)
{
    instance.importManager.SetPackageCjoCache("std.core", allAstData.at("std.core"));
    for (const auto& fileName : fileNames) {
        instance.importManager.SetPackageCjoCache(fileName, allAstData.at(fileName));
    }
}

void VerifyInitialCompile(TestCompilerInstance& instance, Ptr<Package>& curPkg)
{
    bool ret = instance.Compile(CompileStage::IMPORT_PACKAGE);
    EXPECT_TRUE(ret);
    auto depPkgs = instance.importManager.GetAllImportedPackages();
    size_t depPkgsSize = 4; // std.core, vardecl, funcdecl, default package
    EXPECT_EQ(depPkgs.size(), depPkgsSize);
    EXPECT_FALSE(instance.GetSourcePackages().empty());
    curPkg = instance.GetSourcePackages()[0];
    CJC_NULLPTR_CHECK(curPkg);
    EXPECT_FALSE(curPkg->files.empty());
    auto decls = instance.importManager.GetImportedDecls(*curPkg->files.front());
    EXPECT_FALSE(decls.empty());
    bool find = FindDeclInPackage(decls, "classdecl");
    EXPECT_FALSE(find);
}

void VerifyIncrementalCompile(TestCompilerInstance& instance, Ptr<Package>& curPkg)
{
    bool ret = instance.Compile(CompileStage::IMPORT_PACKAGE);
    EXPECT_TRUE(ret);
    auto depPkgs = instance.importManager.GetAllImportedPackages();
    size_t depPkgsSize = 5; // std.core, vardecl, funcdecl, default package, classdecl
    EXPECT_EQ(depPkgs.size(), depPkgsSize);
    auto decls = instance.importManager.GetImportedDecls(*curPkg->files.front());
    EXPECT_FALSE(decls.empty());
    bool find = FindDeclInPackage(decls, "classdecl");
    EXPECT_TRUE(find);
}
}

TEST_F(ImportIncrTest, ImportIncrTest)
{
    diag.ClearError();
    instance = std::make_unique<TestCompilerInstance>(invocation, diag);
    instance->loadSrcFilesFromCache = true;
    SetupPackageCaches(*instance, allAstData, fileNames);
    std::string middleCode = R"(
        import vardecl.*
        import funcdecl.*

        main() {}
    )";
    instance->bufferCache.insert(
        {srcPath + "middle.cj",
         CompilerInstance::SrcCodeCacheInfo(CompilerInstance::SrcCodeChangeState::ADDED, middleCode)});
    Ptr<Package> curPkg;
    VerifyInitialCompile(*instance, curPkg);
    SetupPackageCaches(*instance, allAstData, fileNames);
    std::string updatedMiddleCode = R"(
        import vardecl.*
        import funcdecl.*
        import classdecl.*

        main() {}
    )";
    instance->bufferCache[srcPath + "middle.cj"] =
        CompilerInstance::SrcCodeCacheInfo(CompilerInstance::SrcCodeChangeState::CHANGED, updatedMiddleCode);
    VerifyIncrementalCompile(*instance, curPkg);
}