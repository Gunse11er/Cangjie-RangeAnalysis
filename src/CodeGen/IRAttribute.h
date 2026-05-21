// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CODEGEN_IR_ATTRIBUTE_H
#define CANGJIE_CODEGEN_IR_ATTRIBUTE_H

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"

#include "Utils/CGUtils.h"

namespace Cangjie {
namespace CodeGen {
/********* Attribute adder for llvm::CallBase *********/

inline void AddAttributeAtIndex(llvm::CallBase* callBase, unsigned idx, llvm::Attribute attr)
{
    callBase->addAttributeAtIndex(idx, attr);
}
inline void AddAttributeAtIndex(llvm::CallBase* callBase, unsigned idx, llvm::Attribute::AttrKind kind)
{
    callBase->addAttributeAtIndex(idx, kind);
}

inline void AddParamAttr(llvm::CallBase* callBase, unsigned paramIdx, llvm::Attribute attr)
{
    AddAttributeAtIndex(callBase, llvm::AttributeList::FirstArgIndex + paramIdx, attr);
}
inline void AddParamAttr(llvm::CallBase* callBase, unsigned paramIdx, llvm::Attribute::AttrKind kind)
{
    AddAttributeAtIndex(callBase, llvm::AttributeList::FirstArgIndex + paramIdx, kind);
}

inline void AddRetAttr(llvm::CallBase* callBase, llvm::Attribute attr)
{
    AddAttributeAtIndex(callBase, llvm::AttributeList::ReturnIndex, attr);
}
inline void AddRetAttr(llvm::CallBase* callBase, llvm::Attribute::AttrKind kind)
{
    AddAttributeAtIndex(callBase, llvm::AttributeList::ReturnIndex, kind);
}

inline void AddSRetAttribute(llvm::CallBase* call)
{
    auto retType = call->arg_begin()->get()->getType();
    CJC_ASSERT(retType->isPointerTy());
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    auto type = GetPointerElementType(retType);
    auto sRetAttr = llvm::Attribute::getWithStructRetType(call->getContext(), type);
    AddParamAttr(call, 0, sRetAttr);
#endif
}

inline void SetZExtAttrForCFunc(llvm::CallBase& call)
{
    if (call.getFunctionType()->getReturnType()->isIntegerTy(1)) {
        AddRetAttr(&call, llvm::Attribute::ZExt);
    }
    uint32_t i = 0;
    for (auto arg = call.arg_begin(); arg != call.arg_end(); arg++, i++) {
        if (arg->get()->getType()->isIntegerTy(1)) {
            call.addParamAttr(i, llvm::Attribute::ZExt);
        }
    }
}

/********* Attribute adder for llvm::Function *********/

inline void AddAttributeAtIndex(llvm::Function* func, unsigned idx, llvm::Attribute attr)
{
    func->addAttributeAtIndex(idx, attr);
}
inline void AddAttributeAtIndex(llvm::Function* func, unsigned idx, llvm::Attribute::AttrKind kind)
{
    func->addAttributeAtIndex(idx, llvm::Attribute::get(func->getContext(), kind));
}

inline void AddFnAttr(llvm::Function* func, llvm::Attribute attr)
{
    AddAttributeAtIndex(func, llvm::AttributeList::FunctionIndex, attr);
}
inline void AddFnAttr(llvm::Function* func, llvm::Attribute::AttrKind kind)
{
    AddAttributeAtIndex(func, llvm::AttributeList::FunctionIndex, kind);
}

inline void AddParamAttr(llvm::Function* func, unsigned paramIdx, llvm::Attribute attr)
{
    AddAttributeAtIndex(func, llvm::AttributeList::FirstArgIndex + paramIdx, attr);
}
inline void AddParamAttr(llvm::Function* func, unsigned paramIdx, llvm::Attribute::AttrKind kind)
{
    AddAttributeAtIndex(func, llvm::AttributeList::FirstArgIndex + paramIdx, kind);
}

inline void AddRetAttr(llvm::Function* func, llvm::Attribute attr)
{
    AddAttributeAtIndex(func, llvm::AttributeList::ReturnIndex, attr);
}
inline void AddRetAttr(llvm::Function* func, llvm::Attribute::AttrKind kind)
{
    AddAttributeAtIndex(func, llvm::AttributeList::ReturnIndex, kind);
}

/********* Attribute adder for llvm::Argument *********/

inline void AddSRetAttribute(llvm::Argument* value)
{
    CJC_ASSERT(value->getType()->isPointerTy());
    auto type = GetPointerElementType(value->getType());
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    auto sRet = llvm::Attribute::getWithStructRetType(value->getContext(), type);
    value->addAttr(sRet);
#endif
}

inline void AddByValAttribute(llvm::Argument* value, uint64_t align = 8)
{
    CJC_ASSERT(value->getType()->isPointerTy());
    CJC_ASSERT(GetPointerElementType(value->getType())->isStructTy());
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    auto& llvmCtx = value->getContext();
    auto byValAttr = llvm::Attribute::getWithByValType(llvmCtx, GetPointerElementType(value->getType()));
    value->addAttr(byValAttr);
    auto alignAttr = llvm::Attribute::getWithAlignment(llvmCtx, llvm::Align(align));
    value->addAttr(alignAttr);
#endif
}

inline bool ShouldReturnVoid(const CHIR::Func& func)
{
    return Utils::In(func.GetFuncKind(),
        {
            CHIR::FuncKind::CLASS_CONSTRUCTOR,
            CHIR::FuncKind::PRIMAL_CLASS_CONSTRUCTOR,
            CHIR::FuncKind::STRUCT_CONSTRUCTOR,
            CHIR::FuncKind::PRIMAL_STRUCT_CONSTRUCTOR,
            CHIR::FuncKind::GLOBALVAR_INIT,
            CHIR::FuncKind::FINALIZER,
        });
}

inline void SetSRetAttrForStructReturnType(const CHIR::FuncType& chirFuncTy, llvm::Function& llvmFunc)
{
    bool shouldSetSRet = false;
#ifdef CANGJIE_CODEGEN_CJNATIVE_BACKEND
    if (chirFuncTy.IsCFunc()) {
        // If the function is a CFunc, its SRet attribute has been determined whether to set.
        shouldSetSRet = llvmFunc.hasStructRetAttr();
    } else if (llvmFunc.getReturnType()->isVoidTy()) {
        // Unit and Struct return type will be changed into Void type in IR,
        // and the return value is moved to the first place of the parameters
        shouldSetSRet = true;
    }
#endif

    if (!llvmFunc.arg_empty() && shouldSetSRet) {
        llvmFunc.arg_begin()->addAttr(llvm::Attribute::NoAlias);
        AddSRetAttribute(llvmFunc.arg_begin());
    }
}
} // namespace CodeGen
} // namespace Cangjie

#endif // CANGJIE_CODEGEN_IR_ATTRIBUTE_H
