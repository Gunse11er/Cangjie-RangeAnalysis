// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares pre-typecheck Java interop stage: rewritting of fields to the properties with stubs
 * within @JavaMirror classes.
 *
 * Generated property stubs are replaced with corresponding java calls at later stages.
 *
 */
#ifndef CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_MIRROR_FIELDS
#define CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_MIRROR_FIELDS

#include "JavaInteropManager.h"

namespace Cangjie::Native::FFI::Java {

/**
 * For every field within @JavaMirror class, corresponding property is generated. Original fields are removed.
 */
class RewriteJavaMirrorFields : public PreTypeCheckStage {
public:
    explicit RewriteJavaMirrorFields()
    {
    }
protected:
    void Process(PreTypeCheckContext& ctx) override;
private:
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_REWRITE_JAVA_MIRROR_FIELDS
