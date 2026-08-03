// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_ANALYSIS_VALUE_RANGE_ANALYSIS_H
#define CANGJIE_CHIR_ANALYSIS_VALUE_RANGE_ANALYSIS_H

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cangjie/CHIR/Analysis/BoolDomain.h"
#include "cangjie/CHIR/Analysis/Results.h"
#include "cangjie/CHIR/Analysis/SIntDomain.h"
#include "cangjie/CHIR/Analysis/ValueAnalysis.h"
#include "cangjie/Basic/DiagnosticEngine.h"
#include "cangjie/CHIR/Utils/Utils.h"

namespace Cangjie::CHIR {
class ValueRange {
public:
    enum class RangeKind : uint8_t { BOOL, SINT };

    ValueRange() = delete;

    explicit ValueRange(RangeKind kind);

    virtual ~ValueRange();

    /// join two range, return nullopt if no change happened.
    virtual std::optional<std::unique_ptr<ValueRange>> Join(const ValueRange& rhs) const = 0;

    virtual std::string ToString() const = 0;

    virtual std::unique_ptr<ValueRange> Clone() const = 0;

    /// get range kind, now suppert BOOL or SINT.
    RangeKind GetRangeKind() const;

protected:
    RangeKind kind;
};

class BoolRange : public ValueRange {
public:
    explicit BoolRange(BoolDomain domain);

    ~BoolRange() override = default;

    /// join two range, return nullopt if no change happened.
    std::optional<std::unique_ptr<ValueRange>> Join(const ValueRange& rhs) const override;

    std::string ToString() const override;

    std::unique_ptr<ValueRange> Clone() const override;

    /// get range kind, get BOOL for this range type.
    const BoolDomain& GetVal() const;

private:
    BoolDomain domain;
};

class SIntRange : public ValueRange {
public:
    explicit SIntRange(SIntDomain domain);

    SIntRange(SIntDomain domain, std::optional<std::vector<SInt>> exactValues);

    ~SIntRange() override = default;

    /// join two range, return nullopt if no change happened.
    std::optional<std::unique_ptr<ValueRange>> Join(const ValueRange& rhs) const override;

    std::string ToString() const override;

    std::unique_ptr<ValueRange> Clone() const override;

    /// get range kind, get BOOL for this range type.
    const SIntDomain& GetVal() const;

    const std::optional<std::vector<SInt>>& GetExactValues() const;

private:
    SIntDomain domain;
    std::optional<std::vector<SInt>> exactValues;
};

/**
 * @brief the abstract value domain of range value
 */
using RangeValueDomain = ValueDomain<ValueRange>;
/**
 * @brief the state of range value domain
 */
using RangeDomain = State<RangeValueDomain>;
/**
 * @brief partially specialized analysis import value.
 */
template <> const std::string Analysis<RangeDomain>::name;
template <> const std::optional<unsigned> Analysis<RangeDomain>::blockLimit;
template <> RangeDomain::ChildrenMap ValueAnalysis<RangeValueDomain>::globalChildrenMap;
template <> RangeDomain::AllocatedRefMap ValueAnalysis<RangeValueDomain>::globalAllocatedRefMap;
template <> RangeDomain::AllocatedObjMap ValueAnalysis<RangeValueDomain>::globalAllocatedObjMap;
template <> std::vector<std::unique_ptr<Ref>> ValueAnalysis<RangeValueDomain>::globalRefPool;
template <> std::vector<std::unique_ptr<AbstractObject>> ValueAnalysis<RangeValueDomain>::globalAbsObjPool;
template <> RangeDomain ValueAnalysis<RangeValueDomain>::globalState;

/**
 * @brief heck whether global var need range analysis.
 * @param gv global var to check.
 * @return flag global var need analyse
 */
template <> bool IsTrackedGV<RangeValueDomain>(const GlobalVar& gv);

/**
 * @brief literal value analysis function
 * @param literal input literal value to analyse
 * @return range value literalValue is.
 */
template <> RangeValueDomain HandleNonNullLiteralValue<RangeValueDomain>(const LiteralValue* literal);

/**
 * @brief range analysis for CHIR IR.
 */
class RangeAnalysis final : public ValueAnalysis<RangeValueDomain> {
public:
    using ContextResultVisitor = std::function<void(const Function*, const std::string&, Results<RangeDomain>&)>;

    RangeAnalysis() = delete;
    /**
     * @brief range analysis constructor.
     * @param func function to analyse
     * @param builder CHIR builder for generating IR.
     * @param isDebug flag whether print debug log.
     * @param diag reporter to report warning or error.
     */
    RangeAnalysis(const Function* func, CHIRBuilder& builder, bool isDebug, DiagnosticEngine& diag);

    ~RangeAnalysis() override;

    static bool VisitReachableContextSensitiveResults(
        const Function* rootFunction, Results<RangeDomain>& root, const ContextResultVisitor& visitor);

    static void ClearContextSensitiveResults();

    static std::unique_ptr<ValueRange> GetBoundedLoopObservedRange(const Expression* expression);

    static void ClearBoundedLoopObservedRanges();

    static void SetQueryRefinementContext(
        std::unordered_set<const Block*> blocks, std::unordered_set<const Value*> values);

    static void ClearQueryRefinementBlocks();

    std::unique_ptr<ValueRange> GetLocalBoundedLoopObservedRange(const Expression* expression) const;

    /**
     * @brief get bool domain of CHIR value from state.
     * @param state state to get domain.
     * @param value CHIR value to get domain.
     * @return domain found in state.
     */
    static BoolDomain GetBoolDomainFromState(const RangeDomain& state, const Ptr<Value>& value);

    /**
     * @brief get SInt domain of CHIR value from state.
     * @param state state to get domain.
     * @param value CHIR value to get domain.
     * @return domain found in state.
     */
    static const SIntDomain& GetSIntDomainFromState(const RangeDomain& state, const Ptr<Value>& value);

    /**
     * @brief check this block analyse times, quit analysing in this block if inqueue more than a number.
     * @param block block to check inqueue times.
     * @param curState state to check.
     * @return true if InQueue time exceed the maximum else false.
     */
    bool CheckInQueueTimes(const Block* block, RangeDomain& curState) override;

    unsigned GetNarrowingIterationLimit() const override;

    bool NarrowState(RangeDomain& state, const RangeDomain& candidate) override;

private:
    friend RangeDomain GetTerminatorStateForSuccessor(
        Analysis<RangeDomain>& analysis, const RangeDomain& state,
        const Terminator* terminator, const Block* successor);

    struct ContextAbstractValue;
    struct ContextualSummary;
    struct LambdaContextualSummary;
    using ContextArguments = std::vector<ContextAbstractValue>;
    using ContextGlobalValues = std::vector<std::pair<GlobalVar*, ContextAbstractValue>>;
    using ContextLocationValues = std::vector<std::pair<Value*, ContextAbstractValue>>;
    using BoundedLoopExitCache = std::unordered_map<std::string, ContextLocationValues>;

    RangeAnalysis(const Function* func, CHIRBuilder& builder, bool isDebug, DiagnosticEngine* diag,
        ContextArguments contextArguments, ContextGlobalValues contextGlobalArguments);

    template <class Domain,
        typename = typename std::enable_if<std::is_same_v<Domain, SIntDomain> || std::is_same_v<Domain, BoolDomain>>>
    void PrintDebugMessage(const Ptr<const Expression>& expr, const Domain& domain) const
    {
        std::stringstream ss;
        ss << "[RangeAnalysis] The value of " +
                ExprKindMgr::Instance()->GetKindName(static_cast<size_t>(expr->GetExprKind())) +
                ToPosInfo(expr->GetDebugLocation()) + " has been set to " << domain << "\n";
        std::cout << ss.str();
    }

    void PrintBranchOptMessage(const Ptr<const Expression>& expr, bool isTrueBlockRemained) const;

    // ======== Transfer functions for normal expressions based on ExprMajorKind ======== //

    void HandleNormalExpressionEffect(RangeDomain& state, const Expression* expression) override;

    void HandleUnaryExpr(RangeDomain& state, const UnaryExpression* unaryExpr) const;

    void HandleBinaryExpr(RangeDomain& state, const BinaryExpression* binaryExpr);

    std::optional<SIntDomain> TryComputeSimpleInductionUpdateRange(const BinaryExpression* binaryExpr) const;

    void PreHandleFieldExpr(RangeDomain& state, const Field* field) override;

    void HandleOthersExpr(RangeDomain& state, const Expression* expression);

    void HandleApplyExpr(RangeDomain& state, const Apply* apply, Value* refObj) override;

    void HandleVarStateCapturedByLambda(RangeDomain& state, const Lambda* lambda) override;

    std::optional<Block*> HandleApplyWithExceptionTerminator(
        RangeDomain& state, const ApplyWithException* apply, Value* refObj) override;

    std::optional<Block*> HandleInvokeWithExceptionTerminator(
        RangeDomain& state, const InvokeWithException* invoke, Value* refObj) override;

    void HandleContextSensitiveCall(RangeDomain& state, const Expression* callExpression,
        Value* calleeValue, const std::vector<Value*>& args, Value* result);

    bool TryHandlePureSpawnFutureResult(RangeDomain& state, const Apply* apply);

    bool TryHandlePureSpawnFutureResult(RangeDomain& state, const ApplyWithException* apply);

    const Lambda* ResolvePureSpawnLambdaForApply(const Apply* apply) const;

    const Lambda* ResolvePureSpawnLambdaForApply(const ApplyWithException* apply) const;

    bool IsUniqueSpawnValueProjection(
        Value* future, Value* callee, Type* resultType, Type* parentType) const;

    bool ApplyPureSpawnLambdaResult(
        RangeDomain& state, const Lambda* lambda, Value* result, const Expression* callExpression);

    bool IsPureSpawnLambda(const Lambda* lambda) const;

    void HavocCallEffects(RangeDomain& state, const std::vector<Value*>& args, Value* result,
        const Lambda* lambda = nullptr);

    bool HandleLambdaContextSensitiveCall(
        RangeDomain& state, const Lambda* lambda, const std::vector<Value*>& args, Value* result);

    bool AnalyzeLambdaWithContext(const Lambda* lambda, const std::vector<Value*>& args,
        const RangeDomain& callerState, LambdaContextualSummary& summary);

    std::string BuildLambdaContextKey(
        const RangeDomain& state, const Lambda* lambda, const std::vector<Value*>& args);

    void ApplyLambdaContextSummary(RangeDomain& state, const LambdaContextualSummary& summary,
        const std::vector<Value*>& args, Value* result) const;

    std::optional<ContextAbstractValue> AnalyzeCalleeWithContext(
        const Function* callee, const ContextArguments& arguments,
        std::vector<std::optional<ContextAbstractValue>>& refArgValues, ContextGlobalValues& globalValues,
        const std::string* precomputedKey = nullptr);

    void SummarizeContextOutputs(const Function* callee, const ContextGlobalValues& globals,
        Results<RangeDomain>& results, std::optional<ContextAbstractValue>& returnValue,
        std::vector<std::optional<ContextAbstractValue>>& refArgValues,
        ContextGlobalValues& globalValues);

    static std::string BuildContextKey(
        const Function* callee, const ContextArguments& arguments, const ContextGlobalValues& globalValues);

    ContextAbstractValue CaptureContextValue(
        const RangeDomain& state, Value* value, bool preserveIntervals) const;

    ContextAbstractValue CaptureContextValue(
        const RangeDomain& state, Value* value, Type* type, bool preserveIntervals) const;

    ClassType* GetRecordedContextObjectClass(const Value* object) const;

    void RecordContextObjectClass(const Value* object, ClassType* exactClass) const;

    static ContextAbstractValue JoinContextValues(
        const ContextAbstractValue& lhs, const ContextAbstractValue& rhs);

    ClassType* ResolveExactClassForValue(Value* value) const;

    const Lambda* ResolveContextLambdaForValue(Value* value) const;

    std::optional<std::vector<ClassType*>> ResolveFiniteClassSetForValue(Value* value) const;

    bool HandleFiniteInvokeTargets(
        RangeDomain& state, const Expression* callExpression, const Invoke* invoke);

    bool HandleFiniteInvokeTargets(
        RangeDomain& state, const Expression* callExpression, const InvokeWithException* invoke);

    bool MergeFiniteDispatchTargets(RangeDomain& state, const Expression* callExpression,
        const std::vector<Value*>& args, Value* result, const std::vector<Function*>& targets,
        size_t classCount);

    std::optional<std::vector<GlobalVar*>> CollectContextMutableGlobals(
        const Function* callee, const ContextArguments& arguments) const;

    void ApplyContextValue(RangeDomain& state, Value* dest, const ContextAbstractValue& value) const;

    void ApplyContextValue(RangeDomain& state, Value* dest, Type* type, const ContextAbstractValue& value) const;

    void SeedMutableGlobalInitializers(RangeDomain& state);

    static std::mutex& GetContextSummaryMutex();

    static std::unordered_map<std::string, ContextualSummary>& GetContextSummaryCache();

    static std::vector<std::string>& GetContextSummaryOrder();

    static std::unordered_map<const Function*, size_t>& GetContextCounts();

    static std::unordered_map<const Function*, size_t>& GetBoundedLoopContextCounts();

    static std::mutex& GetBoundedLoopExitCacheMutex();

    static std::unordered_map<const RangeAnalysis*, BoundedLoopExitCache>& GetBoundedLoopExitCaches();

    void RecordTerminatorEdgeState(
        const Terminator* terminator, const Block* successor, const RangeDomain& state);

    const RangeDomain* GetRecordedTerminatorEdgeState(
        const Terminator* terminator, const Block* successor) const;

    // ======================= Transfer functions for terminators ======================= //

    std::optional<Block*> HandleTerminatorEffect(RangeDomain& state, const Terminator* terminator) override;

    std::optional<Block*> HandleBranchTerminator(const RangeDomain& state, const Branch* branch) const;

    std::optional<Block*> HandleMultiBranchTerminator(const RangeDomain& state, const MultiBranch* multi) const;

    std::optional<RangeDomain> TryEvaluateBoundedScalarLoopExit(
        const RangeDomain& state, const Branch* branch, const Block* successor);

    enum class ExceptionKind : uint8_t { SUCCESS, FAIL, NA };

    ExceptionKind HandleTypeCastWithException(RangeDomain& state, const TypeCastWithException* cast);

    ExceptionKind HandleIntOpWithException(RangeDomain& state, const IntOpWithException* intOp);

    // =============== Transfer functions for TypeCast expression =============== //
    SIntDomain ComputeTypeCast(RangeDomain& state, PtrSymbol oldSymbol, const SIntDomain& v, IntWidth dstSize,
        bool dstUnsigned, OverflowStrategy ov) const;

    template <typename TTypeCast> ExceptionKind HandleTypeCast(RangeDomain& state, const TTypeCast* cast)
    {
        auto from = cast->GetSourceTy();
        auto to = cast->GetTargetTy();
        if (from->IsRef() && to->IsRef()) {
            auto object = state.CheckAbstractObjectRefBy(cast->GetSourceValue());
            if (object == nullptr || object->IsTopObjInstance()) {
                state.SetToTopOrTopRef(cast->GetResult(), /* isRef = */ true);
            } else {
                state.SetRefToObject(cast->GetResult(), object, cast);
            }
            return ExceptionKind::NA;
        }
        if (!from->IsInteger() || !to->IsInteger()) {
            state.SetToTopOrTopRef(cast->GetResult(), cast->GetResult()->GetType()->IsRef());
            return ExceptionKind::NA;
        }
        auto value = cast->GetSourceValue();
        const auto& sourceDomain = GetSIntDomainFromState(state, value);
        auto res = ComputeTypeCast(
            state, value, sourceDomain, ToWidth(*to), to->IsUnsignedInteger(), cast->GetOverflowStrategy());
        state.Update(cast->GetResult(), std::make_unique<SIntRange>(res));
        return ExceptionKind::NA;
    }

    BoolDomain GenerateBoolRangeFromBinaryOp(RangeDomain& state, const Ptr<const BinaryExpression>& binaryExpr) const;

    void InitializeFuncEntryState(RangeDomain& state) override;

    DiagnosticEngine* diag;

    ContextArguments contextArguments;

    ContextGlobalValues contextGlobalArguments;

    std::string analysisContextKey;

    bool isContextAnalysis{false};

    mutable std::unordered_map<const Value*, ClassType*> contextObjectClasses;

    std::unordered_map<std::string, std::unique_ptr<LambdaContextualSummary>> lambdaContextSummaries;

    std::unordered_map<const Terminator*, std::unordered_map<const Block*, RangeDomain>> terminatorEdgeStates;

    std::unordered_map<const Expression*, std::unique_ptr<ValueRange>> localBoundedLoopObservations;

    std::unordered_set<std::string> failedBoundedLoopContextAttempts;

    std::unordered_set<const Expression*> incompleteLocalBoundedLoopObservations;

    std::unordered_map<const Block*, uint32_t> inqueueTimes;

    std::unordered_map<const Block*, std::unordered_set<Value*>> queryGuidedWideningValues;

    std::unordered_set<const Block*> queryGuidedWideningFallbackBlocks;
};

    // 构造 RangeAnalysis 在某条终结符后继边上传播的状态。
RangeDomain GetTerminatorStateForSuccessor(
    Analysis<RangeDomain>& analysis, const RangeDomain& state, const Terminator* terminator, const Block* successor);
} // namespace Cangjie::CHIR
#endif
