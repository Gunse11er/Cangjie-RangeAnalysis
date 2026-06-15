// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares a thin bridge to interop runtime and support library
 */

#ifndef CANGJIE_SEMA_OBJ_C_UTILS_INTEROPLIB_BRIDGE_H
#define CANGJIE_SEMA_OBJ_C_UTILS_INTEROPLIB_BRIDGE_H

#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Utils/SafePointer.h"

namespace Cangjie::Interop::ObjC {
class InteropLibBridge {
public:
    explicit InteropLibBridge(ImportManager& importManager, DiagnosticEngine& diag)
        : importManager(importManager), diag(diag)
    {
    }

    /**
     * Gets ForwarderMutex declaration.
     */
    Ptr<AST::TypeAliasDecl> GetForwarderMutexDecl();
    /**
     * Gets ForwarderMutex semantic type (std.sync.Mutex).
     */
    Ptr<AST::Ty> GetForwarderMutexTy();
    /**
     * Gets the constructor decl of ForwarderMutex.
     */
    Ptr<AST::FuncDecl> GetMutexConstructor();
    /**
     * Gets the lock decl of ForwarderMutex.
     */
    Ptr<AST::FuncDecl> GetMutexLock();
    /**
     * Gets the unlock decl of ForwarderMutex.
     */
    Ptr<AST::FuncDecl> GetMutexUnlock();
    /**
     * Gets NativeObjCId declaration.
     * id or Instance Method Pointer type.
     */
    Ptr<AST::TypeAliasDecl> GetNativeObjCIdDecl();

    Ptr<AST::InterfaceDecl> GetObjCIdDecl();

    /**
     * Gets NativeObjCId semantic type.
     */
    Ptr<AST::Ty> GetNativeObjCIdTy();

    /**
     * Gets NativeObjCSel declaration.
     * SEL.
     */
    Ptr<AST::TypeAliasDecl> GetNativeObjCSelDecl();

    /**
     * Gets NativeObjCClass declaration.
     * objc_class*.
     */
    Ptr<AST::TypeAliasDecl> GetNativeObjCClassDecl();

    Ptr<AST::Ty> GetNativeObjCClassTy();

    /**
     * Gets NativeObjCSuperPtr (CPointer<NativeObjCSuper>) declaration.
     * objc_super*.
     */
    Ptr<AST::TypeAliasDecl> GetNativeObjCSuperPtrDecl();

    Ptr<AST::Ty> GetNativeObjCSuperPtrTy();

    Ptr<AST::FuncDecl> GetGetSuperClassDecl();

    /**
     * Gets RegistryId declaration.
     * An opaque identifier for Cangjie mirror objects.
     */
    Ptr<AST::TypeAliasDecl> GetRegistryIdDecl();

    /**
     * Gets RegistryId semantic type.
     */
    Ptr<AST::Ty> GetRegistryIdTy();

    /**
     * Gets ObjCUnreachableCodeException declaration.
     * An exception that has to be used to mark an unreachable code (e.g call static method on @ObjCMirror interface wrapper).
     */
    Ptr<AST::ClassDecl> GetObjCUnreachableCodeExceptionDecl();

    Ptr<AST::ClassDecl> GetObjCOptionalMethodUnimplementedExceptionDecl();

    Ptr<AST::ClassDecl> GetObjCStaticMethodCallOnIntefaceExceptionDecl();

    Ptr<AST::FuncDecl> GetGetFromRegistryByNativeHandleDecl();

    Ptr<AST::FuncDecl> GetGetFromRegistryByIdDecl();

    Ptr<AST::FuncDecl> GetPutToRegistryDecl();

    Ptr<AST::FuncDecl> GetRemoveFromRegistryDecl();

    Ptr<AST::FuncDecl> GetAllocDecl();

    Ptr<AST::FuncDecl> GetWithAutoreleasePoolDecl();

    Ptr<AST::FuncDecl> GetWithAutoreleasePoolObjDecl();

    Ptr<AST::FuncDecl> GetRegisterNameDecl();

    Ptr<AST::FuncDecl> GetGetInstanceVariableObjDecl();

    Ptr<AST::FuncDecl> GetSetInstanceVariableObjDecl();

    Ptr<AST::FuncDecl> GetGetInstanceVariableDecl();

    Ptr<AST::FuncDecl> GetSetInstanceVariableDecl();

    Ptr<AST::FuncDecl> GetGetClassDecl();

    Ptr<AST::FuncDecl> GetGetProtoDecl();

    Ptr<AST::FuncDecl> GetWithMethodEnvDecl();

    Ptr<AST::FuncDecl> GetWithMethodEnvObjDecl();

    Ptr<AST::FuncDecl> GetObjCMsgSendDecl();

    Ptr<AST::FuncDecl> GetObjCMsgSendSuperDecl();

    Ptr<AST::FuncDecl> GetObjCReleaseDecl();

    Ptr<AST::FuncDecl> GetObjCRespondsToSelectorDecl();

    Ptr<AST::FuncDecl> GetGetProtocolDecl();

    Ptr<AST::FuncDecl> GetObjCRetainDecl();

    Ptr<AST::FuncDecl> GetObjCAutoReleaseDecl();

    Ptr<AST::FuncDecl> GetWithObjCSuperDecl();

    Ptr<AST::FuncDecl> GetObjCIsKindOfClassDecl();

    Ptr<AST::FuncDecl> GetObjCConformsToProtocolDecl();

    Ptr<AST::FuncDecl> GetConvertToNSStringDecl();

    Ptr<AST::FuncDecl> GetDescriptionAsStringDecl();

    /**
     * Get objc.lang.ObjCPointer declaration
    */
    Ptr<AST::StructDecl> GetObjCPointerDecl();
    Ptr<AST::FuncDecl> GetObjCPointerConstructor();
    Ptr<AST::VarDecl> GetObjCPointerPointerField();

    /**
     * Get objc.lang.ObjCFunc declaration
    */
    Ptr<AST::StructDecl> GetObjCFuncDecl();
    Ptr<AST::FuncDecl> GetObjCFuncConstructor();
    Ptr<AST::FuncDecl> GetObjCFuncFPointerAccessor();

    /**
     * Get objc.lang.ObjCBlock declaration
    */
    Ptr<AST::ClassDecl> GetObjCBlockDecl();
    Ptr<AST::FuncDecl> GetObjCBlockConstructorFromObjC();
    Ptr<AST::FuncDecl> GetObjCBlockConstructorFromCangjie();
    Ptr<AST::FuncDecl> GetObjCBlockAbiPointerAccessor();
    Ptr<AST::FuncDecl> GetObjCBlockFPointerAccessor();
    /**
    * Get objc.internal.NativeBlockABI declaration
    */
    Ptr<AST::StructDecl> GetNativeBlockABIDecl();
    Ptr<AST::StructDecl> GetCangjieBlockABIDecl();

    Ptr<AST::FuncDecl> GetObjCStoreLambdaAsBlockDecl();
    Ptr<AST::FuncDecl> GetObjCGetLambdaFromBlockDecl();

    Ptr<AST::FuncDecl> GetObjectGetClassDecl();
    bool IsInteropLibAccessible() const;
    static bool IsInteropLibAccessible(ImportManager& importManager);

private:
    static constexpr auto INTEROPLIB_PACKAGE_NAME = "objc.internal";

    template <AST::ASTKind K = AST::ASTKind::DECL> auto GetInteropLibDecl(const std::string& ident)
    {
        auto decl = importManager.GetImportedDecl(INTEROPLIB_PACKAGE_NAME, ident);
        if (!decl) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_member_not_imported, DEFAULT_POSITION,
                INTEROPLIB_PACKAGE_NAME + std::string(".") + ident);
            return Ptr(AST::As<K>(nullptr));
        }

        CJC_ASSERT(decl && decl->astKind == K);
        return Ptr(AST::StaticAs<K>(decl));
    }

    template <AST::ASTKind K = AST::ASTKind::DECL> auto GetObjCLangDecl(const std::string& ident)
    {
        auto decl = importManager.GetImportedDecl(OBJ_C_LANG_PACKAGE_IDENT, ident);
        if (!decl) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_member_not_imported, DEFAULT_POSITION, ident);
            return Ptr(AST::As<K>(nullptr));
        }

        CJC_ASSERT(decl && decl->astKind == K);
        return Ptr(AST::StaticAs<K>(decl));
    }

    ImportManager& importManager;
    DiagnosticEngine& diag;
};

} // namespace Cangjie::Interop::ObjC

#endif // CANGJIE_SEMA_OBJ_C_UTILS_INTEROPLIB_BRIDGE_H
