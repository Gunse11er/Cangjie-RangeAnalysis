// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the helper context for mocking
 */

#ifndef CANGJIE_SEMA_MOCK_CONTEXT_H
#define CANGJIE_SEMA_MOCK_CONTEXT_H

#include "cangjie/Mangle/BaseMangler.h"

namespace Cangjie {

/*
 * Class to store dependencies needed for mocking transformations
 */
class MockContext final {
public:
    MockContext();

    /*
     * Creates MangerContext and manages its lifetime.
     * Needed for mangling declarations inside of extends
     */
    void PrepareManglerContext(Ptr<AST::Package> pkg);

    ~MockContext();

public:
    BaseMangler mangler;

private:
    std::unordered_map<std::string, std::unique_ptr<ManglerContext>> manglerCtxs;
};

} // namespace Cangjie

#endif // CANGJIE_SEMA_MOCK_CONTEXT_H
