// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the MacroExpander related classes, which provides macro expand capabilities.
 */

#ifndef CANGJIE_MACROEXPAND_H
#define CANGJIE_MACROEXPAND_H

#include <cangjie/AST/Node.h>
#include <list>

#include "cangjie/AST/Node.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/Macro/InvokeUtil.h"
#include "cangjie/Macro/MacroCommon.h"

namespace Cangjie {
class MacroExpansion {
public:
    MacroExpansion(CompilerInstance* ci) : ci(ci)
    {
    }
    void Execute(AST::Package& package);
    void Execute(std::vector<OwnedPtr<AST::Package>>& packages);

    /**
     * Expand macros for a top-level decl node.
     * @param decl The top-level decl node to expand. Can be any type of top-level declaration.
     *             If the decl contains macro calls, those macros will be expanded.
     * @return A vector of expanded top-level decl nodes after macro expansion.
     *         The returned nodes replace the original AST nodes with expanded versions.
     */
    std::vector<OwnedPtr<AST::Decl>> ExpandDecl(OwnedPtr<AST::Decl> decl);
    
    // String format of macro generated Tokens, for pretty print.
    std::vector<std::string> tokensEvalInMacro;

private:
    Ptr<AST::Package> curPackage{nullptr};
    CompilerInstance* ci{nullptr};
    MacroCollector macroCollector;

    /**
     * Collect macro placeholder nodes in a package.
     */
    void CollectMacros(AST::Package& package);

    /**
     * Scan decl nodes for macro calls and populate the collector.
     * @param currentDecls The decl nodes to scan.
     * @param collector The collector to populate with macro call information.
     */
    void CollectMacroCallsInDecls(std::vector<OwnedPtr<AST::Decl>>& currentDecls, MacroCollector& collector);

    /**
     * Replace macro call nodes with expanded AST nodes after evaluation.
     * @param macroCalls The evaluated macro calls to replace (reversed in-place).
     */
    void ReplaceMacroCallsInDecls(std::vector<MacroCall>& macroCalls);

    /**
     * Evaluate macro.
     */
    void EvaluateMacros();

    /**
     * Process macro information after macro expansion.
     */
    void ProcessMacros(AST::Package& package);

    /**
     * Process macro information after macro expansion for a list of macro calls.
     * This is used by ExpandDecl to create position mappings without generating .macrocall files.
     * @param macCalls The macro calls to process.
     */
    void ProcessMacros(std::vector<MacroCall>& macCalls);
    /**
     * Replace AST after macro expansion.
     */
    void ReplaceAST(AST::Package& package);

    /**
     * Replace AST helper.
     */
    void ReplaceEachMacro(MacroCall& macCall);

    /**
     * Check attribute if replaced node is enum case member.
     */
    void CheckReplacedEnumCaseMember(MacroCall& macroNode, PtrVector<AST::Decl>& newNodes) const;
    /**
     * Check node Consistency: if all nodes are T.
     */
    template <typename T>
    void CheckNodesConsistency(
        PtrVector<T>& nodes, PtrVector<AST::Node>& newNodes, VectorTarget<OwnedPtr<T>>& target) const;
    void ReplaceDecls(
        MacroCall& macroNode, PtrVector<AST::Node>& newNodes, VectorTarget<OwnedPtr<AST::Decl>>& target) const;
    void ReplaceParams(MacroCall& macroNode, PtrVector<AST::Node>& newNodes,
        VectorTarget<OwnedPtr<AST::FuncParam>>& target) const;
    /**
     * Check FuncParamList legality.
     */
    void CheckReplacedFuncParamList(
        const MacroCall& macroNode, const VectorTarget<OwnedPtr<AST::FuncParam>>& target) const;
    /**
     * Replace each macro node to target position.
     */
    void ReplaceEachMacroHelper(MacroCall& macroNode, PtrVector<AST::Node>& newNodes) const;
    /**
     * Replace File Node.
     */
    void ReplaceEachFileNode(const AST::File& file);
};
} // namespace Cangjie

#endif
