// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Diags.h"
#include "TypeCheckUtil.h"

using namespace Cangjie;
using namespace Sema;
using namespace TypeCheckUtil;

bool TypeChecker::TypeCheckerImpl::ChkTupleLit(ASTContext& ctx, Ty& target, TupleLit& tl)
{
    if (target.IsAny()) {
        tl.ty = Synthesize(ctx, &tl);
        ReplaceIdealTy(tl);
        return Ty::IsTyCorrect(tl.ty);
    }
    Ptr<Ty> targetTy = UnboxOptionType(&target);
    if (!Ty::IsTyCorrect(targetTy) || !targetTy->IsTuple()) {
        DiagMismatchedTypesWithFoundTy(diag, tl, targetTy->String(), "Tuple");
        tl.ty = TypeManager::GetNonNullTy(tl.ty);
        return false;
    }
    auto tupleTy = StaticCast<TupleTy*>(targetTy);
    auto typeArgs = tupleTy->typeArgs;
    if (typeArgs.size() != tl.children.size()) {
        tl.ty = Synthesize(ctx, &tl);
        ReplaceIdealTy(tl);
        DiagMismatchedTypes(diag, tl, *targetTy);
        return false;
    }
    // If the size of target elemTys and elements are equal, check one by one.
    std::vector<Ptr<Ty>> realElemTys;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        CJC_NULLPTR_CHECK(tl.children[i]);
        if (!Check(ctx, typeArgs[i], tl.children[i].get())) {
            if (Ty::IsTyCorrect(typeArgs[i]) && Ty::IsTyCorrect(tl.children[i]->ty)) {
                DiagMismatchedTypes(diag, *tl.children[i], *typeArgs[i]);
            }
            tl.ty = Synthesize(ctx, &tl);
            ReplaceIdealTy(tl);
            return false;
        } else {
            realElemTys.push_back(tl.children[i]->ty);
        }
    }
    // Should use SetTy(), but have bugs on ideal type, use Join() instead at current stage.
    // TupleLit allow elements been boxed by given target type.
    // Eg. Option<Int64>*Option<Int64> <=> (1,1) or I1*I1 <=> (1,1) where Int64 extends I1 allow box.
    tl.ty = targetTy;
    return true;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynTupleLit(ASTContext& ctx, TupleLit& tl)
{
    std::vector<Ptr<Ty>> elemTy;
    // Synthesize the type of each element.
    for (auto& it : tl.children) {
        if (!it) {
            tl.ty = TypeManager::GetInvalidTy();
            return tl.ty;
        }
        if (!Ty::IsTyCorrect(it->ty)) {
            (void)Synthesize(ctx, it.get());
        }
        ReplaceIdealTy(*it);
        elemTy.push_back(it->ty);
    }
    tl.ty = typeManager.GetTupleTy(elemTy);
    return tl.ty;
}
