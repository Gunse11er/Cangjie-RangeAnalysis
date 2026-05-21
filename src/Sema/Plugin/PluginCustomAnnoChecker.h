// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares PluginCustomAnnoChecker class and APILevel information.
 */

#ifndef PLUGIN_CUSTOM_ANNO_CHECKER_H
#define PLUGIN_CUSTOM_ANNO_CHECKER_H

#include <set>
#include <string>
#include <set>

#include "cangjie/AST/Node.h"
#include "cangjie/AST/NodeX.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/Frontend/CompilerInstance.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Option/Option.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie {
namespace PluginCheck {

/**
 * It should same as cangjie code follow:
 * ```
 * @Annotation
 * public class APILevel {
 *     // since
 *     public let since: String
 *     public let atomicservice: Bool
 *     public let crossplatform: Bool
 *     public let deprecated: ?String
 *     public let form: Bool
 *     public let permission: ?PermissionValue
 *     public let syscap: String
 *     public let throwexception: Bool
 *     public let workerthread: Bool
 *     public let systemapi: Bool
 *     public const init(since!: String, atomicservice!: Bool = false, crossplatform!: Bool = false,
 *         deprecated!: ?String = 0, form!: Bool = false, permission!: ?PermissionValue = None,
 *         syscap!: String = "", throwexception!: Bool = false, workerthread!: Bool = false, systemapi!: Bool = false) {
 *         this.since = since
 *         this.atomicservice = atomicservice
 *         this.crossplatform = crossplatform
 *         this.deprecated = deprecated
 *         this.form = form
 *         this.permission = permission
 *         this.syscap = syscap
 *         this.throwexception = throwexception
 *         this.workerthread = workerthread
 *         this.systemapi = systemapi
 *     }
 * }
 * ```
 */

using LevelType = uint32_t;

/**
 * @brief Structure to hold custom annotation information.
 */
struct PluginCustomAnnoInfo {
    LevelType since{0};
    std::string syscap{""};
    std::optional<bool> hasHideAnno{std::nullopt};
};

using SysCapSet = std::set<std::string>;

class PluginCustomAnnoChecker {
public:
    PluginCustomAnnoChecker(CompilerInstance& ci, DiagnosticEngine& diag, ImportManager& importManager)
        : ci(ci), diag(diag), importManager(importManager)
    {
        ParseOption();
    }

    /**
     * @brief Parse custom annotations from declaration.
     * @param decl Declaration to parse.
     * @param annoInfo Output parameter to store parsed annotation information.
     */
    void Parse(const AST::Decl& decl, PluginCustomAnnoInfo& annoInfo);

    /**
     * @brief Check custom annotations in the package.
     * @param pkg Package to check.
     */
    void Check(AST::Package& pkg);

private:
    void ParseOption() noexcept;
    bool ParseJsonFile(const std::vector<uint8_t>& in) noexcept;
    struct DiagConfig {
        bool reportDiag{true};
        Ptr<AST::Node> node{nullptr};
        std::vector<std::string> message{};
    };
    bool CheckLevel(const AST::Decl& target, const PluginCustomAnnoInfo& scopeAnnoInfo, DiagConfig diagCfg);
    bool CheckSyscap(const AST::Decl& target, const PluginCustomAnnoInfo& scopeAnnoInfo, DiagConfig diagCfg);
    bool CheckCheckingHide(const AST::Decl& target, DiagConfig diagCfg);
    bool CheckNode(Ptr<AST::Node> node, const PluginCustomAnnoInfo& scopeAnnoInfo, bool reportDiag = true);
    void CheckIfAvailableExpr(AST::IfAvailableExpr& iae, const PluginCustomAnnoInfo& scopeAnnoInfo);
    bool IsAnnoAPILevel(Ptr<AST::Annotation> anno, const AST::Decl& decl);
    bool IsAnnoHide(Ptr<AST::Annotation> anno);
    void ParseHideArg(const AST::Annotation& anno, PluginCustomAnnoInfo& annoInfo);
    void ParseAPILevelArgs(const AST::Decl& decl, const AST::Annotation& anno, PluginCustomAnnoInfo& annoInfo);
    void CheckHideOfExtendDecl(const AST::Decl& decl, const PluginCustomAnnoInfo& annoInfo);
    void CheckHideOfOverrideFunction(const AST::Decl& decl, const PluginCustomAnnoInfo& annoInfo);
    void CheckAnnoBeforeMacro(AST::Package& pkg);

private:
    CompilerInstance& ci;
    DiagnosticEngine& diag;
    ImportManager& importManager;
    Ptr<ASTContext> ctx;

    LevelType globalLevel{0};
    SysCapSet intersectionSet;
    SysCapSet unionSet;
    std::unordered_map<Ptr<const AST::Decl>, PluginCustomAnnoInfo> levelCache;
    std::string curModuleName{""};

    bool optionWithLevel{false};
    bool optionWithSyscap{false};
};
} // namespace PluginCheck
} // namespace Cangjie

#endif
