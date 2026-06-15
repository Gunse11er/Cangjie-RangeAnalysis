// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares pre-typecheck logics for Java interop declarations (@JavaMirror, @JavaImpl).
 */
#ifndef CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_INTEROP_MANAGER
#define CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_INTEROP_MANAGER

#include "cangjie/Utils/SafePointer.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie::Native::FFI::Java {

struct PreTypeCheckContext {
public:
    explicit PreTypeCheckContext(const ImportManager& importManager, TypeManager& typeManager, AST::Package& pkg);

    void AddGeneratedDecl(OwnedPtr<AST::Decl>&& decl);

    void FlushGeneratedDecls();

    const ImportManager& importManager;
    TypeManager& typeManager;
    AST::Package& pkg;
    const std::vector<Ptr<AST::ClassDecl>> javaImpls;
    const std::vector<Ptr<AST::ClassLikeDecl>> javaMirrors;

private:
    std::vector<OwnedPtr<AST::Decl>> generated;
};

class PreTypeCheckStage {
public:
    void operator()(PreTypeCheckContext& ctx);
protected:
    virtual void Process(PreTypeCheckContext& ctx) = 0;
    virtual ~PreTypeCheckStage()
    {
    }
};

class JavaInteropManager {
public:
    explicit JavaInteropManager(const ImportManager& importManager, TypeManager& typeManager);

    /**
     * Process java interoperability related checks, generations, and transformations.
     */
    void Process(AST::Package& pkg) const;

private:
    template <typename S, class... StageArgs>
    std::enable_if_t<std::is_base_of_v<PreTypeCheckStage, S>, void>
    Process(PreTypeCheckContext& ctx, StageArgs&&... args) const
    {
        S stage(std::forward<StageArgs>(args)...);
        stage(ctx);
    }

    const ImportManager& importManager;
    TypeManager& typeManager;
};
} // namespace Cangjie::Native::FFI::Java

#endif // CANGJIE_SEMA_PRE_TYPECHECK_NATIVE_FFI_JAVA_DESUGAR_INTEROP_MANAGER
