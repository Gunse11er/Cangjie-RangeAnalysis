// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_FRONTEND_FRONTENDOBSERVER_H
#define CANGJIE_FRONTEND_FRONTENDOBSERVER_H

#include <algorithm>
#include <vector>

#include "cangjie/Frontend/CompilerInstance.h"

namespace Cangjie {
/// A simple observer of frontend actions.
class FrontendObserver {
public:
    FrontendObserver() = default;
    virtual ~FrontendObserver() = default;

    /// An event, triggerd when the frontend has parsed AST.
    virtual void ParsedAST(CompilerInstance& instance) = 0;
};

/// List of FrontendObserver.
class MultiFrontendObserver : public FrontendObserver {
public:
    /// Make a clone of \ref observer and register it.
    void Add(FrontendObserver* observer) { observers.push_back(std::unique_ptr<FrontendObserver>(observer)); }

    /// An event, triggerd when the frontend has parsed AST.
    void ParsedAST(CompilerInstance& instance) override
    {
        std::for_each(observers.begin(), observers.end(),
            [&instance](const auto& observer) { observer->ParsedAST(instance); });
    };

private:
    std::vector<std::unique_ptr<FrontendObserver>> observers;
};
} // namespace Cangjie

#endif // CANGJIE_FRONTEND_FRONTENDOBSERVER_H
