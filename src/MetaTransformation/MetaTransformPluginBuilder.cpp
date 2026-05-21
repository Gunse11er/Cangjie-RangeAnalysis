// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
#include "cangjie/MetaTransformation/MetaTransform.h"

using namespace Cangjie;

CHIRPluginManager MetaTransformPluginBuilder::BuildCHIRPluginManager(CHIR::CHIRBuilder& builder)
{
    CHIRPluginManager chirPluginManager;
    for (auto& callback : chirPluginCallbacks) {
        callback(chirPluginManager, builder);
    }
    return chirPluginManager;
}
#endif
