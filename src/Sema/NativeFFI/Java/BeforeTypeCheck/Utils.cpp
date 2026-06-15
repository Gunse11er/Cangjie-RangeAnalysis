// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Utils.h"
#include "NativeFFI/Utils.h"
#include "cangjie/AST/AttributePack.h"
#include "cangjie/AST/Create.h"
#include "TypeCheckUtil.h"
#include "cangjie/Utils/CheckUtils.h"

using namespace Cangjie;
using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;
using namespace Cangjie::Native::FFI;

namespace Cangjie::Native::FFI::Java {
using namespace Cangjie::TypeCheckUtil;

namespace {
void CloneVisibilityAttribute(const Decl& from, Node& to)
{
    if (from.TestAttr(Attribute::PUBLIC)) {
        to.EnableAttr(Attribute::PUBLIC);
    } else if (from.TestAttr(Attribute::INTERNAL)) {
        to.EnableAttr(Attribute::INTERNAL);
    } else if (from.TestAttr(Attribute::PROTECTED)) {
        to.EnableAttr(Attribute::PROTECTED);
    } else if (from.TestAttr(Attribute::PRIVATE)) {
        to.EnableAttr(Attribute::PRIVATE);
    }
}
}

OwnedPtr<ClassDecl> CloneClassSkeleton(ClassLikeDecl& sample, std::string&& name)
{
    auto cd = WithinFile(MakeOwned<ClassDecl>(), sample.curFile);
    CloneVisibilityAttribute(sample, *cd);

    cd->EnableAttr(Attribute::COMPILER_ADD);
    cd->identifier = std::move(name);
    cd->identifier.SetPos(sample.identifier.Begin(), sample.identifier.End());

    cd->fullPackageName = sample.curFile->curPackage->fullPackageName;
    cd->begin = sample.begin;
    cd->end = sample.end;

    cd->body = WithinFile(MakeOwned<ClassBody>(), sample.curFile);
    cd->body->begin = sample.begin;
    cd->body->end = sample.end;
    cd->moduleName = Cangjie::Utils::GetRootPackageName(sample.fullPackageName);

    return cd;
}

void InsertMethodStub(FuncDecl& fd, const ImportManager& importManager, TypeManager& typeManager)
{
    CJC_ASSERT(fd.funcBody);
    auto argTy = GetStringDecl(importManager).GetTy();
    auto arg = CreateLitConstExpr(LitConstKind::STRING, "It's compiler generated stub.", argTy);
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(arg));

    static auto& exception = GetExceptionDecl(importManager);
    auto throwExpr = CreateThrowException(exception, std::move(args), *fd.curFile, typeManager);

    std::vector<OwnedPtr<Node>> nodes;
    nodes.emplace_back(std::move(throwExpr));

    fd.funcBody->body = CreateBlock(std::move(nodes));
}

ClassDecl& GetExceptionDecl(const ImportManager& importManager)
{
    const auto exceptionDecl = importManager.GetCoreDecl("Exception");
    CJC_NULLPTR_CHECK(exceptionDecl);

    ClassDecl* exception = nullptr;
    if (auto ex = As<ASTKind::CLASS_DECL>(exceptionDecl)) {
        exception = ex;
    }
    CJC_NULLPTR_CHECK(exception);

    return *exception;
}

} // namespace Cangjie::Native::FFI::Java
