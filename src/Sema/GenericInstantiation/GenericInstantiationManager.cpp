// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Implements GenericInstantiationManager API calls.
 * Real implementation functions in 'GenericInstantiationManagerImpl'.
 */
#include "GenericInstantiationManagerImpl.h"

#include "cangjie/Frontend/CompilerInstance.h"

using namespace Cangjie;

GenericInstantiationManager::GenericInstantiationManager(CompilerInstance& ci)
{
    impl = std::make_unique<GenericInstantiationManagerImpl>(ci);
}

GenericInstantiationManager::~GenericInstantiationManager()
{
}

void GenericInstantiationManager::GenericInstantiatePackage(AST::Package& pkg) const
{
    impl->GenericInstantiatePackage(pkg);
}

Ptr<AST::Decl> GenericInstantiationManager::GetInstantiatedDeclWithGenericInfo(
    const GenericInfo& genericInfo, AST::Package& pkg) const
{
    return impl->GetInstantiatedDeclWithGenericInfo(genericInfo, pkg);
}
std::unordered_set<Ptr<AST::Decl>> GenericInstantiationManager::GetInstantiatedDecls(
    const AST::Decl& genericDecl) const
{
    return impl->GetInstantiatedDecls(genericDecl);
}

void GenericInstantiationManager::ResetGenericInstantiationStage() const
{
    impl->ResetGenericInstantiationStage();
}

Generic2InsMap GenericInstantiationManager::GetAllGenericToInsDecls() const
{
    return impl->GetAllGenericToInsDecls();
}