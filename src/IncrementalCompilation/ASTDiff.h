// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CACHE_DATA_ASTDIFF_IMPL_H
#define CANGJIE_CACHE_DATA_ASTDIFF_IMPL_H

#include "cangjie/AST/Node.h"
#include "cangjie/IncrementalCompilation/ASTCacheCalculator.h"
#include "cangjie/IncrementalCompilation/IncrementalScopeAnalysis.h"
#include "cangjie/IncrementalCompilation/Utils.h"
#include "cangjie/IncrementalCompilation/IncrementalCompilationLogger.h"
#include "cangjie/Modules/ImportManager.h"

namespace Cangjie::IncrementalCompilation {

constexpr int DELIMITER_NUM{60};

// function, variable, or property change
struct CommonChange {
    Ptr<const AST::Decl> decl;
    bool sig;
    bool srcUse;
    bool body;

    // returns true if there is any change
    explicit operator bool() const
    {
        return sig || srcUse || body;
    }

    friend std::ostream& operator<<(std::ostream& out, const CommonChange& m)
    {
        out << m.decl->rawMangleName << ": ";
        if (!m) {
            return out << "no change\n";
        }
        if (m.sig) {
            out << "sig ";
        }
        if (m.srcUse) {
            out << "srcuse ";
        }
        if (m.body) {
            out << "body ";
        }
        return out << '\n';
    }
};

struct TypeChange {
    bool instVar;
    bool virtFun;
    bool sig;
    bool srcUse;
    bool body;
    bool order;

    std::list<CommonChange> changed;
    std::list<Ptr<const AST::Decl>> added; // added non-virtual functions and properties, include extended ones
    std::list<RawMangledName> del;
    explicit operator bool() const
    {
        return instVar || virtFun || sig || srcUse || body || order || !changed.empty() || !added.empty() ||
            !del.empty();
    }

    friend std::ostream& operator<<(std::ostream& out, const TypeChange& m)
    {
        if (!m) {
            return out << "no change\n";
        }
        if (m.instVar) {
            out << "memory ";
        }
        if (m.virtFun) {
            out << "virtual ";
        }
        if (m.sig) {
            out << "sig ";
        }
        if (m.srcUse) {
            out << "srcuse ";
        }
        if (m.body) {
            out << "body ";
        }
        out << '\n';
        if (!m.added.empty()) {
            out << "    added members " << m.added.size() << ": ";
            for (auto decl : m.added) {
                out << decl->rawMangleName << ' ';
            }
            out << '\n';
        }
        if (!m.del.empty()) {
            out << "    deleted members " << m.del.size() << ": ";
            for (auto& d : m.del) {
                out << d << ' ';
            }
            out << '\n';
        }
        if (!m.changed.empty()) {
            out << "    changed members " << m.changed.size() << ":\n";
            for (auto& change : m.changed) {
                out << "         " << change;
            }
        }
        return out;
    }
};

struct ModifiedDecls {
    // added top level decls
    std::list<const AST::Decl*> added;
    // all deleted decls goes here

    std::list<RawMangledName> deletes;
    bool import{false}; // change of import hash
    bool args{false};   // change of compile args

    // changed top level decls begin here:
    std::unordered_map<Ptr<const AST::InheritableDecl>, TypeChange> types;
    std::unordered_map<Ptr<const AST::Decl>, CommonChange> commons; // changed top level variable and functions
    std::list<Ptr<const AST::TypeAliasDecl>> aliases;
    std::list<RawMangledName> deletedTypeAlias;
    std::list<const AST::Decl*> orderChanges;
    operator bool() const
    {
        return !added.empty() || !deletedTypeAlias.empty() || !deletes.empty() || !types.empty() || !commons.empty() ||
            !orderChanges.empty() || !aliases.empty();
    }

    void Dump() const
    {
        auto& logger = IncrementalCompilationLogger::GetInstance();
        if (!logger.IsEnable()) {
            return;
        }
        if (!operator bool()) {
            logger.LogLn("no raw modified decls");
            return;
        }
        std::stringstream out;
        for (int i{0}; i < DELIMITER_NUM; ++i) {
            out << '=';
        }
        out << "\nbegin dump raw modified decls:\n";
        for (auto a : ToSortedPointers(added, [](auto a, auto b) { return a->begin < b->begin; })) {
            CJC_NULLPTR_CHECK(a);
            out << "added ";
            if (!a->identifier.Empty()) { // skip empty identifier, i.e. in extend
                out << a->identifier.Val() << " ";
            }
            out << a->rawMangleName << " at " << a->identifier.Begin().line << ',' << a->identifier.Begin().column
                << '\n';
        }
        for (auto& d : ToSorted(deletedTypeAlias)) {
            out << "deleted " << *d << '\n';
        }
        for (auto& d : ToSorted(deletes)) {
            out << "deleted " << *d << '\n';
        }
        for (auto &t : ToSorted(types, [](auto&a, auto&b) { return a.first->begin < b.first->begin; })) {
            if (t->second) {
                PrintDecl(out, *t->first);
                out << ": " << t->second;
            }
        }
        for (auto &t : ToSorted(commons, [](auto&a, auto&b) { return a.first->begin < b.first->begin; })) {
            if (t->second) {
                out << t->second;
            }
        }
        if (!orderChanges.empty()) {
            out << orderChanges.size() << " order changed decl(s).\n";
        }
        for (auto& t: ToSortedPointers(orderChanges, [](auto a, auto b) { return a->begin < b->begin; })) {
            CJC_NULLPTR_CHECK(t);
            out << "order change " << t->rawMangleName << '\n';
        }
        for (int i{0}; i < DELIMITER_NUM; ++i) {
            out << '=';
        }
        // flush for debugging purpose
        logger.LogLn(out.str());
    }
};

struct ASTDiffArgs {
    const CompilationCache& prevCache;
    const ASTCache& curImports;
    RawMangled2DeclMap importedMangled2Decl;
    const RawMangled2DeclMap& rawMangleName2DeclMap;
    const ASTCache& astCacheInfo;
    const FileMap& curFileMap;
    const GlobalOptions& op;
};
struct ASTDiffResult {
    ModifiedDecls changedDecls;
    RawMangled2DeclMap mangled2Decl;
};

ASTDiffResult ASTDiff(ASTDiffArgs&& args);
} // namespace Cangjie::IncrementalCompilation

#endif
