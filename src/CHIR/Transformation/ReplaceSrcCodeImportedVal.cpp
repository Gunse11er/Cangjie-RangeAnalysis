// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Transformation/ReplaceSrcCodeImportedVal.h"

#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/Utils/ProfileRecorder.h"

using namespace Cangjie::CHIR;

ReplaceSrcCodeImportedVal::ReplaceSrcCodeImportedVal(Package& package)
    : package(package)
{
}

void ReplaceSrcCodeImportedVal::RemoveUselessDefFromCC(
    const std::unordered_set<ClassDef*>& uselessClasses, const std::unordered_set<Function*>& uselessLambda)
{
    Utils::ProfileRecorder temp("ReplaceSrcCodeImportedVal", "RemoveUselessDefFromCC");
    std::unordered_set<Function*> toBeRemovedFuncs;
    for (auto lambda : uselessLambda) {
        for (auto user : lambda->GetUsers()) {
            user->RemoveSelfFromBlock();
        }
        lambda->DestroySelf();
        toBeRemovedFuncs.emplace(lambda);
    }

    for (auto def : uselessClasses) {
        for (auto func : def->GetMethods()) {
            for (auto user : func->GetUsers()) {
                user->RemoveSelfFromBlock();
            }
            func->DestroySelf();
            toBeRemovedFuncs.emplace(func);
        }
    }
    std::vector<ClassDef*> newClasses;
    auto classes = package.GetClasses();
    for (auto def : classes) {
        if (uselessClasses.find(def) == uselessClasses.end()) {
            newClasses.emplace_back(def);
        }
    }
    package.SetClasses(std::move(newClasses));
    
    std::vector<Function*> globalFuncs;
    for (auto func : package.GetGlobalFunctions()) {
        if (toBeRemovedFuncs.find(func) != toBeRemovedFuncs.end()) {
            continue;
        }
        globalFuncs.emplace_back(func);
    }
    package.SetAllGlobalFuncs(std::move(globalFuncs));
}

void ReplaceSrcCodeImportedVal::Run(const std::unordered_set<Function*>& srcCodeImportedFuncs,
    const std::unordered_set<GlobalVar*>& srcCodeImportedVars, const std::unordered_set<ClassDef*>& uselessClasses,
    const std::unordered_set<Function*>& uselessLambda)
{
    Utils::ProfileRecorder recorder("CHIR", "ReplaceSrcCodeImportedVal");
    // 1. destroy func body and var's initializer
    for (auto func : srcCodeImportedFuncs) {
        func->DestroyFuncBody();
    }
    std::unordered_set<Function*> toBeRemovedFuncs;
    for (auto var : srcCodeImportedVars) {
        if (auto initFunc = var->GetInitFunc()) {
            toBeRemovedFuncs.emplace(initFunc);
        }
        var->DestroyInitializer();
    }
    std::vector<Function*> newFuncs;
    for (auto func : package.GetGlobalFunctions()) {
        if (toBeRemovedFuncs.find(func) != toBeRemovedFuncs.end()) {
            continue;
        }
        newFuncs.emplace_back(func);
    }
    package.SetAllGlobalFuncs(std::move(newFuncs));

    // 2. remove useless def that created in closure conversion
    RemoveUselessDefFromCC(uselessClasses, uselessLambda);
}
