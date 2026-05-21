// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements FFI typecheck apis.
 */

#include "TypeCheckerImpl.h"

#include "cangjie/AST/ASTContext.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"

using namespace Cangjie;
using namespace Cangjie::AST;

void TypeChecker::TypeCheckerImpl::PreCheckAnnoForFFI(Node& root)
{
    if (root.TestAttr(Attribute::IMPORTED)) {
        return;
    }
    Walker walker(&root, [this](const Ptr<Node> node) -> VisitAction {
        if (node && node->IsDecl()) {
            auto decl = StaticCast<Decl*>(node);
            SetForeignABIAttr(*decl);
            PreCheckAnnoForCFFI(*decl);
        }
        return VisitAction::WALK_CHILDREN;
    });
    walker.Walk();
}
