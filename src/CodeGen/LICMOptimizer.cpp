// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LICMOptimizer.h"

#include "CGModule.h"
#include "IRBuilder.h"
#include "Utils/CGCommonDef.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

namespace Cangjie::CodeGen {
void LICMOptimizer::Run()
{
    if (cgMod.GetCGContext().GetCompileOptions().optimizationLevel < GlobalOptions::OptimizationLevel::O2 ||
        cgFunc.GetRawFunction()->isDeclaration()) {
        return;
    }
    MoveLoopInvariant4GetMethodOuterTI();
}

void LICMOptimizer::MoveLoopInvariant4GetMethodOuterTI()
{
    auto virtualCallInfo4LICMMap = cgMod.GetCGContext().GetVirtualCallInfo4LICM(&cgFunc);
    if (virtualCallInfo4LICMMap.empty()) {
        return;
    }

    // Build a set of live BasicBlocks to avoid quadratic lookups
    auto& bbList = cgFunc.GetRawFunction()->getBasicBlockList();
    std::unordered_set<llvm::BasicBlock*> bbSet;
    bbSet.reserve(std::distance(bbList.begin(), bbList.end()));
    for (auto& bb : bbList) {
        if (bb.getName().startswith("prepForVirtualCall.bb")) {
            bbSet.insert(&bb);
        }
    }
    // Filter out the basicBlocks that may be deleted
    std::vector<VirtualCallInfo4LICM> filteredInfo;
    filteredInfo.reserve(virtualCallInfo4LICMMap.size());
    for (const auto& info : virtualCallInfo4LICMMap) {
        if (bbSet.count(info.prepForVirtualCallBB)) {
            filteredInfo.emplace_back(info);
        }
    }
    if (filteredInfo.empty()) {
        return;
    }

    IRBuilder2 irBuilder(cgMod);
    irBuilder.SetInsertCGFunction(cgFunc);
    for (const auto& virtualCallInfo4LICM : filteredInfo) {
        auto prepForVirtualCallEndBB = virtualCallInfo4LICM.prepForVirtualCallBB->getUniqueSuccessor();
        CJC_NULLPTR_CHECK(prepForVirtualCallEndBB);
        if (!prepForVirtualCallEndBB) {
            continue;
        }

        auto lookupVirtualInst = llvm::cast<llvm::Instruction>(virtualCallInfo4LICM.lookupVirtualInst);
        if (auto loop = GetMostOuterLoopFor(*lookupVirtualInst); loop) {
            llvm::BasicBlock* preHdr = loop->getLoopPreheader();
            if (!preHdr) {
                continue;
            }
            llvm::BasicBlock* uniqueSuccessor = preHdr->getUniqueSuccessor();
            if (!uniqueSuccessor || !llvm::isa<llvm::BranchInst>(preHdr->getTerminator())) {
                continue;
            }

            auto [vmCachePtr, outerTICachePtr] = GenCreateCacheLogic(irBuilder, preHdr, virtualCallInfo4LICM);
            GenCheckAndUseCacheLogic(irBuilder, vmCachePtr, outerTICachePtr, virtualCallInfo4LICM);
        }
    }
}

std::pair<llvm::Value*, llvm::Value*> LICMOptimizer::GenCreateCacheLogic(
    IRBuilder2& irBuilder, llvm::BasicBlock* preHdr, const VirtualCallInfo4LICM& virtualCallInfo4LICM)
{
    auto i8PtrTy = irBuilder.getInt8PtrTy();
    auto preHdrSuccessor = preHdr->getUniqueSuccessor();

    // create and store cache
    irBuilder.SetInsertPoint(&cgFunc.GetRawFunction()->getEntryBlock());
    auto vmCachePtr = irBuilder.CreateEntryAlloca(i8PtrTy, nullptr, "virtual_method_cache");
    auto outerTICachePtr = virtualCallInfo4LICM.getOuterTypeInst
        ? irBuilder.CreateEntryAlloca(i8PtrTy, nullptr, "outer_Type_cache")
        : nullptr;
    irBuilder.SetInsertPoint(preHdr);
    if (auto terminator = preHdr->getTerminator()) {
        terminator->eraseFromParent();
    }
    auto thisTI = irBuilder.CreateTypeInfo(virtualCallInfo4LICM.thisType);
    auto isRef = irBuilder.CreateTypeInfoIsReferenceCall(thisTI);
    auto [refBB, nonRefBB] = Vec2Tuple<2>(
        irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("thisTI.ref"), GenNameForBB("thisTI.non.ref")}));
    irBuilder.LLVMIRBuilder2::CreateCondBr(isRef, refBB, nonRefBB);

    irBuilder.SetInsertPoint(refBB);
    irBuilder.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy), vmCachePtr);
    irBuilder.LLVMIRBuilder2::CreateBr(preHdrSuccessor);

    irBuilder.SetInsertPoint(nonRefBB);
    auto introType = irBuilder.CreateTypeInfo(virtualCallInfo4LICM.outerType);
    auto idxValue = irBuilder.getInt64(virtualCallInfo4LICM.virtualMethodOffset);
    auto vmCache = irBuilder.CallIntrinsicMTable({thisTI, introType, idxValue});
    irBuilder.CreateStore(vmCache, vmCachePtr);
    if (outerTICachePtr) {
        auto outerTypeCache = irBuilder.CallIntrinsicMethodOuterType({thisTI, introType, idxValue});
        irBuilder.CreateStore(outerTypeCache, outerTICachePtr);
    }
    irBuilder.LLVMIRBuilder2::CreateBr(preHdrSuccessor);

    return {vmCachePtr, outerTICachePtr};
}

void LICMOptimizer::GenCheckAndUseCacheLogic(IRBuilder2& irBuilder, llvm::Value* vmCachePtr,
    llvm::Value* outerTICachePtr, const VirtualCallInfo4LICM& virtualCallInfo4LICM)
{
    auto i8PtrTy = irBuilder.getInt8PtrTy();
    auto lookupVirtualInst = virtualCallInfo4LICM.lookupVirtualInst;
    auto getOuterTypeInst = virtualCallInfo4LICM.getOuterTypeInst;
    auto prepForVirtualCallBB = virtualCallInfo4LICM.prepForVirtualCallBB;
    auto prepForVirtualCallEndBB = prepForVirtualCallBB->getUniqueSuccessor();

    // gen check cache logic
    auto oldBB = prepForVirtualCallBB->splitBasicBlock(&*prepForVirtualCallBB->begin(), "prepForVirtualCall.bb.old");
    irBuilder.SetInsertPoint(prepForVirtualCallBB);
    if (auto terminator = prepForVirtualCallBB->getTerminator()) {
        terminator->eraseFromParent();
    }
    auto vmCacheLoaded = irBuilder.CreateLoad(i8PtrTy, vmCachePtr);
    auto isNull = irBuilder.CreateICmpEQ(vmCacheLoaded, llvm::ConstantPointerNull::get(i8PtrTy));
    auto [nonNullBB] = Vec2Tuple<1>(irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("cache.non.null")}));
    irBuilder.LLVMIRBuilder2::CreateCondBr(isNull, oldBB, nonNullBB);
    irBuilder.SetInsertPoint(nonNullBB);
    auto outerTICacheLoaded = outerTICachePtr ? irBuilder.CreateLoad(i8PtrTy, outerTICachePtr) : nullptr;
    irBuilder.LLVMIRBuilder2::CreateBr(prepForVirtualCallEndBB);

    // gen replace uses logic
    irBuilder.SetInsertPoint(prepForVirtualCallEndBB, prepForVirtualCallEndBB->begin());
    auto vmPHI = irBuilder.CreatePHI(i8PtrTy, 2U);
    lookupVirtualInst->replaceAllUsesWith(vmPHI);
    vmPHI->addIncoming(lookupVirtualInst, oldBB);
    vmPHI->addIncoming(vmCacheLoaded, nonNullBB);
    if (getOuterTypeInst) {
        auto outerTIPHI = irBuilder.CreatePHI(i8PtrTy, 2U);
        getOuterTypeInst->replaceAllUsesWith(outerTIPHI);
        outerTIPHI->addIncoming(getOuterTypeInst, oldBB);
        outerTIPHI->addIncoming(outerTICacheLoaded, nonNullBB);
    }
}

llvm::Loop* LICMOptimizer::GetMostOuterLoopFor(const llvm::Instruction& inst)
{
    llvm::LoopInfoBase<llvm::BasicBlock, llvm::Loop>* loopInfo;
    if (dtAndLoopInfo) {
        loopInfo = dtAndLoopInfo->loopInfo.get();
    } else {
        auto dtUPtr = std::make_unique<llvm::DominatorTree>(*cgFunc.GetRawFunction());
        auto loopInfoUPtr = std::make_unique<llvm::LoopInfoBase<llvm::BasicBlock, llvm::Loop>>();
        loopInfoUPtr->analyze(*dtUPtr);
        loopInfo = loopInfoUPtr.get();
        dtAndLoopInfo = std::make_unique<DTAndLoopInfo>(std::move(dtUPtr), std::move(loopInfoUPtr));
    }
    auto loop = loopInfo->getLoopFor(inst.getParent());
    return loop ? loop->getOutermostLoop() : nullptr;
}
} // namespace Cangjie::CodeGen
