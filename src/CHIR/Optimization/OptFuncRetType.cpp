// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Optimization/OptFuncRetType.h"
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "cangjie/Utils/CheckUtils.h"

using namespace Cangjie::CHIR;

namespace {
void RemoveOldRetValue(LocalVar& oldRet)
{
    /*  remove this kind of code:
        %1: Unit& = Allocate(Unit)  // old ret value
        ...
        %2: Unit = Constant(Unit)
        %3: Unit& = Store(%2, %1)

        we are not sure if the Store is the only user of the old ret value, if there is Load,
        we can't remove the old ret value. The following condition is a little simple, but it's enough for now.
    */
    for (auto user : oldRet.GetUsers()) {
        if (user->GetExprKind() != ExprKind::STORE) {
            return;
        }
    }
    for (auto user : oldRet.GetUsers()) {
        auto store = Cangjie::StaticCast<Store*>(user);
        if (auto unitVal = Cangjie::DynamicCast<LocalVar*>(store->GetValue())) {
            /*  we only care about the following code:
                %1: Unit& = Allocate(Unit)  // old ret value
                ...
                %2: Unit = Constant(Unit)
                %3: Unit& = Store(%2, %1)

                we can't remove store's operand if in following code:
                %1: Unit& = Allocate(Unit)  // old ret value
                ...
                %2: Unit = Apply(xxx, ...)
                %3: Unit& = Store(%2, %1)

                there may be other safety cases that you can remove the store's operand, you can add them here.
            */
            if (unitVal->GetExpr()->IsConstant()) {
                unitVal->GetExpr()->RemoveSelfFromBlock();
            }
        }
        store->RemoveSelfFromBlock();
    }
    oldRet.GetExpr()->RemoveSelfFromBlock();
}
}

OptFuncRetType::OptFuncRetType(Package& package, CHIRBuilder& builder) : package(package), builder(builder)
{
}

void OptFuncRetType::Unit2Void()
{
    // 1. collect all global functions that should return Void
    for (auto func : package.GetGlobalFunctions()) {
        if (!ReturnTypeShouldBeVoid(*func)) {
            continue;
        }
        CJC_ASSERT(func->GetReturnType()->IsUnit());
        LocalVar* oldRet = nullptr;
        // 2. change the return type to Void
        if (func->IsFuncWithBody()) {
            oldRet = func->GetReturnValue();
            CJC_NULLPTR_CHECK(oldRet);
        }
        func->ReplaceReturnValue(nullptr, builder);
        // 3. remove the old ret value, just for clean code
        if (oldRet != nullptr) {
            RemoveOldRetValue(*oldRet);
        }
        // 4. replace all call sites with the new return type
        for (auto user : func->GetUsers()) {
            if (auto apply = DynamicCast<Apply*>(user)) {
                CJC_ASSERT(apply->GetCallee() == func);
                auto funcCallContext = FuncCallContext{
                    .args = apply->GetArgs(),
                    .instTypeArgs = apply->GetInstantiatedTypeArgs(),
                    .thisType = apply->GetThisType()
                };
                auto newApply = builder.CreateExpression<Apply>(
                    apply->GetDebugLocation(), func->GetReturnType(), apply->GetCallee(), funcCallContext, apply->GetParentBlock());
                if (apply->IsSuperCall()) {
                    newApply->SetSuperCall();
                }
                apply->ReplaceWith(*newApply);
            } else {
                auto awe = StaticCast<ApplyWithException*>(user);
                CJC_ASSERT(awe->GetCallee() == func);
                auto funcCallContext = FuncCallContext{
                    .args = awe->GetArgs(),
                    .instTypeArgs = awe->GetInstantiatedTypeArgs(),
                    .thisType = awe->GetThisType()
                };
                auto newApply = builder.CreateExpression<ApplyWithException>(
                    awe->GetDebugLocation(), func->GetReturnType(), awe->GetCallee(), funcCallContext,
                    awe->GetSuccessBlock(), awe->GetErrorBlock(), awe->GetParentBlock());
                awe->ReplaceWith(*newApply);
            }
        }
    }
}