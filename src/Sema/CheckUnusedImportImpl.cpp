// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file defines functions for check unused import.
 */

#include "TypeCheckerImpl.h"
#include "cangjie/Utils/Utils.h"
#include "cangjie/Utils/ProfileRecorder.h"

using namespace Cangjie;
using namespace AST;

namespace {
using UsedPackageMap = std::unordered_map<std::string, std::unordered_set<std::pair<std::string, Ptr<Decl>>, HashPair>>;

class CheckUnusedImportImpl {
public:
    CheckUnusedImportImpl(Package& pkg, DiagnosticEngine& diag, ImportManager& importManager)
        : pkg(pkg), diag(diag), importManager(importManager)
    {
    }

    ~CheckUnusedImportImpl() = default;
    void Check();

private:
    void CollectUsedPackages(Node& node);
    void CollectNeedCheckImports();
    bool IsImportContentUsed(AST::ImportSpec& importSpec);
    bool IsImportContentUsedInMacro(AST::ImportSpec& importSpec);
    void AddUsedExtendDeclTarget(const Ptr<AST::ExtendDecl> ed, UsedPackageMap& usedMap) const;
    void AddUsedTargetWithIdentifier(Node& node, const std::string& identifier, Ptr<Decl> target);
    void AddUsedTarget(Node& node, Ptr<Decl> target);
    void AddUsedPackage(Node& node);
    void ReportUnusedImports();
    UsedPackageMap& GetUsedPackageMap(ImportSpec& importSpec);
    UsedPackageMap& GetCacheUsedPackageMap(ImportSpec& importSpec);
    bool CheckImportAllUsed(ImportSpec& importSpec, const std::string& packageName,
        UsedPackageMap& usedPackage,
        UsedPackageMap& cacheUsedPackage,
        const std::map<std::string, OrderedDeclSet>& declsMap);
    bool CheckDeclImportUsed(ImportSpec& importSpec, const std::string& packageName,
        UsedPackageMap& usedPackage,
        UsedPackageMap& cacheUsedPackage,
        const std::map<std::string, OrderedDeclSet>& declsMap);
    bool CheckPackageImportUsed(ImportSpec& importSpec, const std::string& packageName,
        const std::unordered_set<std::pair<std::string, Ptr<Decl>>, HashPair>& usedDecls);
    void UpdateCacheForDecl(ImportSpec& importSpec, const std::string& packageName, const std::string& identifier,
        Ptr<Decl> decl);
    UsedPackageMap usedPackageInAST;
    std::unordered_map<Ptr<File>, UsedPackageMap> usedPackageInFile;
    UsedPackageMap cacheUsedPackageInAST;
    std::unordered_map<Ptr<File>, UsedPackageMap> cacheUsedPackageInFile;
    std::vector<Ptr<ImportSpec>> needCheckImport;
    Package& pkg;
    DiagnosticEngine& diag;
    ImportManager& importManager;
};

std::string GetRefIdentifier(Node& node)
{
    if (node.astKind == ASTKind::REF_TYPE) {
        return StaticCast<RefType>(&node)->ref.identifier.Val();
    }
    if (node.astKind == ASTKind::REF_EXPR) {
        return StaticCast<RefExpr>(&node)->ref.identifier.Val();
    }
    return "";
}

} // namespace

void CheckUnusedImportImpl::AddUsedExtendDeclTarget(
    const Ptr<AST::ExtendDecl> ed, UsedPackageMap& usedMap) const
{
    Ptr<Decl> target = nullptr;
    for (auto& type : ed->inheritedTypes) {
        if (target = type->GetTarget(); target) {
            usedMap[target->GetFullPackageName()].emplace(std::make_pair(target->identifier.Val(), target));
        }
    }
    if (ed->extendedType && ed->extendedType->GetTarget()) {
        target = ed->extendedType->GetTarget();
        usedMap[target->GetFullPackageName()].emplace(std::make_pair(target->identifier.Val(), target));
    }

    if (!ed->generic) {
        return;
    }

    for (auto& gc : ed->generic->genericConstraints) {
        for (auto& ub : gc->upperBounds) {
            if (target = ub->GetTarget(); target) {
                usedMap[target->GetFullPackageName()].emplace(std::make_pair(target->identifier.Val(), target));
            }
        }
    }
}

void CheckUnusedImportImpl::AddUsedTargetWithIdentifier(Node& node, const std::string& identifier, Ptr<Decl> target)
{
    auto& foundInAST = usedPackageInAST[target->fullPackageName];
    foundInAST.emplace(std::make_pair(identifier, target));
    if (target->outerDecl != nullptr) {
        foundInAST.emplace(std::make_pair(target->outerDecl->identifier.Val(), target->outerDecl));
        if (auto ed = DynamicCast<ExtendDecl>(target->outerDecl); ed != nullptr) {
            AddUsedExtendDeclTarget(ed, usedPackageInAST);
        }
    }

    if (!node.curFile) {
        return;
    }

    auto& fileUsed = usedPackageInFile[node.curFile];
    auto& foundInFile = fileUsed[target->fullPackageName];

    foundInFile.emplace(std::make_pair(identifier, target));
    if (target->outerDecl != nullptr) {
        foundInFile.emplace(std::make_pair(target->outerDecl->identifier.Val(), target->outerDecl));
        if (auto ed = DynamicCast<ExtendDecl>(target->outerDecl); ed != nullptr) {
            AddUsedExtendDeclTarget(ed, fileUsed);
        }
    }
}

void CheckUnusedImportImpl::AddUsedTarget(Node& node, Ptr<Decl> target)
{
    const std::string& identifier = GetRefIdentifier(node);
    if (!identifier.empty()) {
        AddUsedTargetWithIdentifier(node, identifier, target);
    } else {
        AddUsedTargetWithIdentifier(node, target->identifier.Val(), target);
    }
}

void CheckUnusedImportImpl::AddUsedPackage(Node& node)
{
    auto target = node.GetTarget();
    if (target == nullptr) {
        return;
    }

    AddUsedTarget(node, target);

    auto targets = node.GetTargets();
    for (auto decl : targets) {
        AddUsedTarget(node, decl);
    }
}

void CheckUnusedImportImpl::CollectNeedCheckImports()
{
    for (auto& file : pkg.files) {
        for (auto& import : file->imports) {
            // IMPLICIT_ADD(std.core) or cjmp scenes does not need to be checked.
            if (import->TestAttr(AST::Attribute::IMPLICIT_ADD) || import->begin.IsZero() || import->end.IsZero()) {
                continue;
            }
            // multi-import or reExport scenes does not need to be checked.
            auto modifier = import->modifier ? import->modifier->modifier : TokenKind::PRIVATE;
            if (modifier == TokenKind::PUBLIC || modifier == TokenKind::PROTECTED || import->IsImportMulti()) {
                continue;
            }
            bool isInternalNeedExport =
                import->curFile && import->curFile->curPackage && !import->curFile->curPackage->noSubPkg;
            if (modifier == TokenKind::INTERNAL && isInternalNeedExport) {
                continue;
            }
            needCheckImport.emplace_back(import.get());
        }
    }
}

void CheckUnusedImportImpl::CollectUsedPackages(Node& node)
{
    Walker walker(&node, nullptr, [this](Ptr<Node> node) -> VisitAction {
        if (node->astKind == ASTKind::IMPORT_SPEC) {
            return VisitAction::SKIP_CHILDREN;
        }
        AddUsedPackage(*node);
        return VisitAction::WALK_CHILDREN;
    });

    walker.Walk();
}

bool CheckUnusedImportImpl::IsImportContentUsedInMacro(AST::ImportSpec& importSpec)
{
    CJC_ASSERT(importSpec.curFile);
    auto cjoManager = importManager.GetCjoManager();
    const std::string& packageName = cjoManager->GetPackageNameByImport(importSpec);

    auto usedMacroInFile = importManager.GetUsedMacroDecls(*importSpec.curFile);
    auto declsMap = cjoManager->GetPackageMembers(packageName);
    if (importSpec.IsImportAll() || !importSpec.content.isDecl) {
        if (!usedMacroInFile[packageName].empty()) {
            return true;
        }
        for (auto [_, decls] : declsMap) {
            for (auto decl : decls) {
                if (usedMacroInFile[decl->fullPackageName].count(decl) > 0) {
                    cacheUsedPackageInFile[importSpec.curFile][packageName].emplace(
                        std::make_pair(decl->identifier.Val(), decl));
                    return true;
                }
            }
        }
    } else {
        auto decls = declsMap[importSpec.content.identifier];
        for (auto decl : decls) {
            if (usedMacroInFile[decl->fullPackageName].count(decl) > 0) {
                cacheUsedPackageInFile[importSpec.curFile][packageName].emplace(
                    std::make_pair(decl->identifier.Val(), decl));
                return true;
            }
        }
    }

    return false;
}

UsedPackageMap& CheckUnusedImportImpl::GetUsedPackageMap(
    ImportSpec& importSpec)
{
    return (importSpec.IsPrivateImport() && importSpec.curFile) ? usedPackageInFile[importSpec.curFile]
                                                                : usedPackageInAST;
}

UsedPackageMap& CheckUnusedImportImpl::GetCacheUsedPackageMap(
    ImportSpec& importSpec)
{
    return (importSpec.IsPrivateImport() && importSpec.curFile) ? cacheUsedPackageInFile[importSpec.curFile]
                                                                : cacheUsedPackageInAST;
}

void CheckUnusedImportImpl::UpdateCacheForDecl(ImportSpec& importSpec, const std::string& packageName,
    const std::string& identifier, Ptr<Decl> decl)
{
    cacheUsedPackageInAST[packageName].emplace(std::make_pair(identifier, decl));
    if (importSpec.curFile) {
        cacheUsedPackageInFile[importSpec.curFile][packageName].emplace(std::make_pair(identifier, decl));
    }
}

bool CheckUnusedImportImpl::CheckImportAllUsed(ImportSpec& importSpec, const std::string& packageName,
    UsedPackageMap& usedPackage,
    UsedPackageMap& cacheUsedPackage,
    const std::map<std::string, OrderedDeclSet>& declsMap)
{
    if (!cacheUsedPackage[packageName].empty()) {
        return true;
    }

    for (auto [_, decls] : declsMap) {
        for (auto decl : decls) {
            auto& usedDecls = usedPackage[decl->fullPackageName];
            auto declPair = std::make_pair(decl->identifier.Val(), decl);
            if (usedDecls.find(declPair) != usedDecls.end()) {
                UpdateCacheForDecl(importSpec, packageName, decl->identifier.Val(), decl);
                return true;
            }
        }
    }
    return false;
}

bool CheckUnusedImportImpl::CheckDeclImportUsed(ImportSpec& importSpec, const std::string& packageName,
    UsedPackageMap& usedPackage, UsedPackageMap& cacheUsedPackage,
    const std::map<std::string, OrderedDeclSet>& declsMap)
{
    std::string identifier =
        importSpec.IsImportAlias() ? importSpec.content.aliasName.Val() : importSpec.content.identifier.Val();
    auto decls = declsMap.find(importSpec.content.identifier);
    if (decls == declsMap.end()) {
        return false;
    }

    for (auto decl : decls->second) {
        auto declPair = std::make_pair(identifier, decl);
        if (cacheUsedPackage[packageName].find(declPair) != cacheUsedPackage[packageName].end()) {
            return true;
        }

        auto& usedDecls = usedPackage[decl->fullPackageName];
        if (usedDecls.find(declPair) != usedDecls.end()) {
            UpdateCacheForDecl(importSpec, packageName, identifier, decl);
            return true;
        }
    }
    return false;
}

bool CheckUnusedImportImpl::CheckPackageImportUsed(ImportSpec& importSpec, const std::string& packageName,
    const std::unordered_set<std::pair<std::string, Ptr<Decl>>, HashPair>& usedDecls)
{
    std::string identifier =
        importSpec.IsImportAlias() ? importSpec.content.aliasName.Val() : importSpec.content.identifier.Val();
    auto packageDecl = importManager.GetPackageDecl(packageName);
    auto packagePair = std::make_pair(identifier, packageDecl);
    return usedDecls.find(packagePair) != usedDecls.end();
}

bool CheckUnusedImportImpl::IsImportContentUsed(ImportSpec& importSpec)
{
    auto cjoManager = importManager.GetCjoManager();
    const std::string& packageName = cjoManager->GetPackageNameByImport(importSpec);

    UsedPackageMap& usedPackage = GetUsedPackageMap(importSpec);

    const auto& usedDecls = usedPackage[packageName];
    if (!usedDecls.empty() && importSpec.IsImportAll()) {
        return true;
    }

    auto& cacheUsedPackage = GetCacheUsedPackageMap(importSpec);
    auto declsMap = cjoManager->GetPackageMembers(packageName);

    if (importSpec.IsImportAll()) {
        if (CheckImportAllUsed(importSpec, packageName, usedPackage, cacheUsedPackage, declsMap)) {
            return true;
        }
    } else if (importSpec.content.isDecl) {
        if (CheckDeclImportUsed(importSpec, packageName, usedPackage, cacheUsedPackage, declsMap)) {
            return true;
        }
    } else {
        if (CheckPackageImportUsed(importSpec, packageName, usedDecls)) {
            return true;
        }
    }

    auto package = cjoManager->GetPackage(packageName);
    if (package && package->isMacroPackage) {
        return IsImportContentUsedInMacro(importSpec);
    }
    return false;
}

void CheckUnusedImportImpl::ReportUnusedImports()
{
    for (auto& importSpec : needCheckImport) {
        if (importSpec->TestAttr(Attribute::IMPORTED) && importSpec->TestAttr(Attribute::FROM_COMMON_PART)) {
            continue;
        }
        if (IsImportContentUsed(*importSpec)) {
            continue;
        }
        diag.DiagnoseRefactor(DiagKindRefactor::sema_unused_import, MakeRange(importSpec->begin, importSpec->end),
            importSpec->content.ToString());
    }
}

void CheckUnusedImportImpl::Check()
{
    CollectUsedPackages(pkg);
    CollectNeedCheckImports();
    ReportUnusedImports();
}

void TypeChecker::TypeCheckerImpl::CheckUnusedImportSpec(Package& pkg)
{
    Utils::ProfileRecorder recorder("Post TypeCheck", "CheckUnusedImportSpec");
    CheckUnusedImportImpl(pkg, diag, importManager).Check();
}
