// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Analysis/ValueRangeAnalysis.h"

#include <climits>
#include <limits>
#include <mutex>
#include <unordered_set>
#include "cangjie/CHIR/Checker/OverflowChecking.h"
#include "cangjie/CHIR/Analysis/Arithmetic.h"

namespace Cangjie::CHIR {
namespace {
using LoopRangeSnapshot = std::unordered_map<Value*, std::unique_ptr<SIntDomain>>;
using LoopRangeSnapshots = std::unordered_map<const Block*, LoopRangeSnapshot>;
std::unordered_map<const RangeAnalysis*, LoopRangeSnapshots> loopRangeSnapshots;
std::mutex loopRangeSnapshotsMtx;
}

ValueRange::ValueRange(RangeKind kind) : kind(kind)
{
}

ValueRange::~ValueRange()
{
}

ValueRange::RangeKind ValueRange::GetRangeKind() const
{
    return kind;
}

BoolRange::BoolRange(BoolDomain domain) : ValueRange(RangeKind::BOOL), domain(std::move(domain))
{
}

std::optional<std::unique_ptr<ValueRange>> BoolRange::Join(const ValueRange& rhs) const
{
    CJC_ASSERT(rhs.GetRangeKind() == RangeKind::BOOL);
    auto rhsRange = StaticCast<const BoolRange&>(rhs);
    if (domain.IsSame(rhsRange.domain)) {
        return std::nullopt;
    }
    return std::make_unique<BoolRange>(BoolRange{BoolDomain::Union(domain, rhsRange.domain)});
}

std::string BoolRange::ToString() const
{
    std::stringstream ss;
    ss << domain;
    return ss.str();
}

std::unique_ptr<ValueRange> BoolRange::Clone() const
{
    return std::make_unique<BoolRange>(domain);
}

const BoolDomain& BoolRange::GetVal() const
{
    return domain;
}

SIntRange::SIntRange(SIntDomain domain) : ValueRange(RangeKind::SINT), domain(std::move(domain))
{
}

std::optional<std::unique_ptr<ValueRange>> SIntRange::Join(const ValueRange& rhs) const
{
    CJC_ASSERT(rhs.GetRangeKind() == RangeKind::SINT);
    auto rhsRange = StaticCast<const SIntRange&>(rhs);
    if (!domain.IsSame(rhsRange.domain) ||
        (domain.NumericBound().IsFullSet() && !domain.SymbolicBounds().Empty())) {
        return std::make_unique<SIntRange>(SIntRange{SIntDomain::Unions(domain, rhsRange.domain)});
        }
    return std::nullopt;
}

std::string SIntRange::ToString() const
{
    std::stringstream ss;
    ss << domain;
    return ss.str();
}

std::unique_ptr<ValueRange> SIntRange::Clone() const
{
    return std::make_unique<SIntRange>(domain);
}

const SIntDomain& SIntRange::GetVal() const
{
    return domain;
}

template <> bool IsTrackedGV<RangeValueDomain>(const GlobalVar& gv)
{
    auto baseTyKind = StaticCast<RefType*>(gv.GetType())->GetBaseType()->GetTypeKind();
    return (baseTyKind >= Type::TYPE_INT8 && baseTyKind <= Type::TYPE_UINT_NATIVE) || baseTyKind == Type::TYPE_ENUM ||
        baseTyKind == Type::TYPE_BOOLEAN;
}

template <> RangeValueDomain HandleNonNullLiteralValue<RangeValueDomain>(const LiteralValue* literal)
{
    if (literal->IsBoolLiteral()) {
        return RangeValueDomain(
            std::make_unique<BoolRange>(BoolDomain::FromBool(StaticCast<BoolLiteral*>(literal)->GetVal())));
    } else if (literal->IsIntLiteral()) {
        return RangeValueDomain(std::make_unique<SIntRange>(SIntDomain::From(*literal)));
    } else {
        return RangeValueDomain(true);
    }
}

// 初始化 RangeAnalysis，并保存诊断器以便表达式 transfer 报告算术错误。
RangeAnalysis::RangeAnalysis(const Func* func, CHIRBuilder& builder, bool isDebug, const Ptr<DiagAdapter>& diag)
    : ValueAnalysis(func, builder, isDebug), diag(diag)
{
}

// 析构时清理当前分析实例对应的循环快照。
RangeAnalysis::~RangeAnalysis()
{
    std::lock_guard<std::mutex> lock(loopRangeSnapshotsMtx);
    loopRangeSnapshots.erase(this);
}

const int LOOP_WIDENING_START_TIMES = 4;
const int LOOP_WIDENING_SNAPSHOT_START_TIMES = LOOP_WIDENING_START_TIMES - 1;
const int MAX_INQUEUE_TIMES = 32;

bool CanAnalyse(const Ptr<Type>& type)
{
    if (type->IsInteger() || type->IsBoolean()) {
        return true;
    }
    return false;
}

const SIntDomain& GetDefaultIntCache(const Ptr<Type>& ty)
{
    constexpr int integerSize{4};
    static SIntDomain signedRange[integerSize]{
        {ConstantRange::Full(IntWidth::I8), false},
        {ConstantRange::Full(IntWidth::I16), false},
        {ConstantRange::Full(IntWidth::I32), false},
        {ConstantRange::Full(IntWidth::I64), false},
    };
    static SIntDomain unsignedRange[integerSize]{
        {ConstantRange::Full(IntWidth::I8), true},
        {ConstantRange::Full(IntWidth::I16), true},
        {ConstantRange::Full(IntWidth::I32), true},
        {ConstantRange::Full(IntWidth::I64), true},
    };
    auto width{Ctz(static_cast<unsigned>(ToWidth(*ty)) / static_cast<unsigned>(CHAR_BIT))};
    return ty->IsUnsignedInteger() ? unsignedRange[width] : signedRange[width];
}

// 按指定符号语义比较两个 SInt 是否严格小于。
bool StrictlyLess(const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    return isUnsigned ? lhs.Ult(rhs) : lhs.Slt(rhs);
}

// 按指定符号语义比较两个 SInt 是否严格大于。
bool StrictlyGreater(const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    return isUnsigned ? lhs.Ugt(rhs) : lhs.Sgt(rhs);
}

// 对整数区间执行单边 widening，保证循环不动点收敛。
SIntDomain WidenSIntDomain(const SIntDomain& previous, const SIntDomain& current)
{
    auto width = current.Width();
    auto isUnsigned = current.IsUnsigned();
    const auto& previousNumeric = previous.NumericBound();
    const auto& currentNumeric = current.NumericBound();
    if (currentNumeric.IsEmptySet() || currentNumeric.IsFullSet()) {
        return current;
    }
    if (previousNumeric.IsEmptySet() || previousNumeric.IsFullSet()) {
        return current;
    }
    if (currentNumeric.IsWrappedSet() || currentNumeric.IsSignWrappedSet() || previousNumeric.IsWrappedSet() ||
        previousNumeric.IsSignWrappedSet()) {
        return SIntDomain::Top(width, isUnsigned);
    }

    auto previousMin = previousNumeric.MinValue(isUnsigned);
    auto previousMax = previousNumeric.MaxValue(isUnsigned);
    auto currentMin = currentNumeric.MinValue(isUnsigned);
    auto currentMax = currentNumeric.MaxValue(isUnsigned);
    auto lowerMovedDown = StrictlyLess(currentMin, previousMin, isUnsigned);
    auto upperMovedUp = StrictlyGreater(currentMax, previousMax, isUnsigned);
    if (lowerMovedDown && upperMovedUp) {
        return SIntDomain::Top(width, isUnsigned);
    }
    if (upperMovedUp) {
        return SIntDomain::FromNumeric(RelationalOperation::GE, currentMin, isUnsigned);
    }
    if (lowerMovedDown) {
        return SIntDomain::FromNumeric(RelationalOperation::LE, currentMax, isUnsigned);
    }
    return current;
}

struct LoopWidenCandidate {
    Value* value;
    Type* type;
    bool preserveDuringBodyWidening;
};

// 判断后继路径是否可以回到候选循环头。
bool CanReachBlockForWidening(const Block* start, const Block* target)
{
    if (start == nullptr || target == nullptr) {
        return false;
    }
    std::vector<const Block*> worklist{start};
    std::unordered_set<const Block*> visited;
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (!visited.emplace(block).second) {
            continue;
        }
        if (block == target) {
            return true;
        }
        for (auto successor : block->GetSuccessors()) {
            worklist.emplace_back(successor);
        }
    }
    return false;
}

// 识别适合作为 widening 头部的循环分支块。
bool IsLoopHeaderForWidening(const Block* block)
{
    if (block == nullptr) {
        return false;
    }
    auto terminator = block->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return false;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    if (CanReachBlockForWidening(branch->GetTrueBlock(), block)) {
        return true;
    }
    return CanReachBlockForWidening(branch->GetFalseBlock(), block);
}

// 判断表达式类型是否是可用作循环 guard 的关系比较。
bool IsRelationalExprKindForWidening(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
        case ExprKind::LE:
        case ExprKind::GT:
        case ExprKind::GE:
        case ExprKind::EQUAL:
        case ExprKind::NOTEQUAL:
            return true;
        default:
            return false;
    }
}

// 获取 widening 逻辑使用的局部值定义表达式。
const Expression* GetDefiningExprForWidening(Value* value)
{
    auto local = DynamicCast<LocalVar*>(value);
    if (local == nullptr) {
        return nullptr;
    }
    return local->GetExpr();
}

// 判断某个值是否是循环 guard 直接使用的 load。
bool IsLoopConditionLoadForWidening(Value* value)
{
    auto expr = GetDefiningExprForWidening(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return false;
    }
    auto header = expr->GetParentBlock();
    if (!IsLoopHeaderForWidening(header)) {
        return false;
    }
    auto branch = StaticCast<const Branch*>(header->GetTerminator());
    auto condExpr = GetDefiningExprForWidening(branch->GetCondition());
    if (condExpr == nullptr || condExpr->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR ||
        !IsRelationalExprKindForWidening(condExpr->GetExprKind())) {
        return false;
    }
    auto binary = StaticCast<const BinaryExpression*>(condExpr);
    return binary->GetLHSOperand() == value || binary->GetRHSOperand() == value;
}

// 判断循环体 widening 时是否应保留 guard load 的精度。
bool ShouldPreserveLoopGuardValueDuringBodyWidening(const Block* block, Value* value)
{
    auto expr = GetDefiningExprForWidening(value);
    if (expr == nullptr || expr->GetParentBlock() == nullptr || expr->GetParentBlock() == block) {
        return false;
    }
    return IsLoopConditionLoadForWidening(value);
}

// 从状态中读取整数域，缺失时返回类型全域。
const SIntDomain& GetSIntDomainFromState(const RangeDomain& state, Value* value, Type* type)
{
    CJC_ASSERT(type != nullptr && type->IsInteger());
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return GetDefaultIntCache(type);
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return GetDefaultIntCache(type);
    }
    return StaticCast<const SIntRange*>(absVal)->GetVal();
}

// 将带类型的整数值加入循环 widening 候选集。
void AddWidenCandidate(std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value,
    Type* type, bool preserveDuringBodyWidening = false)
{
    if (value == nullptr || type == nullptr || !type->IsInteger() || seen.find(value) != seen.end()) {
        return;
    }
    seen.emplace(value);
    candidates.emplace_back(LoopWidenCandidate{value, type, preserveDuringBodyWidening});
}

// 将整数值按自身类型加入循环 widening 候选集。
void AddWidenCandidate(std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value,
    bool preserveDuringBodyWidening = false)
{
    AddWidenCandidate(candidates, seen, value, value == nullptr ? nullptr : value->GetType(), preserveDuringBodyWidening);
}

// 将 ref 背后的整数抽象对象加入 widening 候选集。
void AddReferencedIntegerObjectCandidate(
    RangeDomain& state, std::vector<LoopWidenCandidate>& candidates, std::unordered_set<Value*>& seen, Value* value)
{
    if (value == nullptr || !value->GetType()->IsRef()) {
        return;
    }
    auto refType = StaticCast<RefType*>(value->GetType());
    if (!refType->GetRootBaseType()->IsInteger()) {
        return;
    }
    AddWidenCandidate(candidates, seen, state.CheckAbstractObjectRefBy(value), refType->GetRootBaseType());
}

// 收集 block 中可能需要循环 widening 的整数值和 ref 对象。
std::vector<LoopWidenCandidate> CollectLoopWidenCandidates(RangeDomain& state, const Block* block)
{
    std::vector<LoopWidenCandidate> candidates;
    std::unordered_set<Value*> seen;
    for (auto expr : block->GetExpressions()) {
        AddWidenCandidate(candidates, seen, expr->GetResult());
        for (auto operand : expr->GetOperands()) {
            AddWidenCandidate(candidates, seen, operand,
                ShouldPreserveLoopGuardValueDuringBodyWidening(block, operand));
        }
        switch (expr->GetExprKind()) {
            case ExprKind::LOAD: {
                auto load = StaticCast<const Load*>(expr);
                AddReferencedIntegerObjectCandidate(state, candidates, seen, load->GetLocation());
                break;
            }
            case ExprKind::STORE: {
                auto store = StaticCast<const Store*>(expr);
                AddReferencedIntegerObjectCandidate(state, candidates, seen, store->GetLocation());
                break;
            }
            default:
                break;
        }
    }
    return candidates;
}

// 在 widening 前捕获当前 block 的整数值域快照。
LoopRangeSnapshot CaptureLoopRangeSnapshot(RangeDomain& state, const Block* block)
{
    LoopRangeSnapshot snapshot;
    for (auto [value, type, preserveDuringBodyWidening] : CollectLoopWidenCandidates(state, block)) {
        (void)preserveDuringBodyWidening;
        snapshot.emplace(value, std::make_unique<SIntDomain>(GetSIntDomainFromState(state, value, type)));
    }
    return snapshot;
}

// 查询上一轮循环快照中的值域，缺失时回退到类型全域。
const SIntDomain& GetPreviousLoopRange(const LoopRangeSnapshot& previousRanges, Value* value, Type* type)
{
    auto it = previousRanges.find(value);
    if (it == previousRanges.end()) {
        return GetDefaultIntCache(type);
    }
    return *it->second;
}

// 对重复访问的循环 block 应用候选整数值 widening。
void ApplyLoopWidening(RangeDomain& state, const LoopRangeSnapshot& previousRanges, const Block* block)
{
    for (auto [value, type, preserveDuringBodyWidening] : CollectLoopWidenCandidates(state, block)) {
        if (preserveDuringBodyWidening) {
            continue;
        }
        const auto& current = GetSIntDomainFromState(state, value, type);
        const auto& previous = GetPreviousLoopRange(previousRanges, value, type);
        auto widened = WidenSIntDomain(previous, current);
        if (!widened.IsSame(current)) {
            state.Update(value, std::make_unique<SIntRange>(std::move(widened)));
        }
    }
}

inline bool IsBasicBinaryExpr(const Expression& expr)
{
    return expr.GetExprKind() >= ExprKind::ADD && expr.GetExprKind() <= ExprKind::MOD;
}

// 判断是否是整数位运算二元表达式。
inline bool IsBitwiseBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::BITAND || kind == ExprKind::BITOR || kind == ExprKind::BITXOR;
}

// 判断是否是整数移位二元表达式。
inline bool IsShiftBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::LSHIFT || kind == ExprKind::RSHIFT;
}

// 判断是否是布尔逻辑二元表达式。
inline bool IsLogicalBinaryExpr(ExprKind kind)
{
    return kind == ExprKind::AND || kind == ExprKind::OR;
}

// 根据闭区间端点构造 ConstantRange。
ConstantRange RangeFromMinMax(const SInt& min, const SInt& max, bool isUnsigned)
{
    auto lower = ConstantRange::From(RelationalOperation::GE, min, !isUnsigned);
    auto upper = ConstantRange::From(RelationalOperation::LE, max, !isUnsigned);
    return lower.IntersectWith(upper, PreferFromBool(isUnsigned));
}

// 根据溢出策略计算整数一元负号的值域。
SIntDomain ComputeNegRange(const SIntDomain& operand, const Ptr<Type>& type, OverflowStrategy ov)
{
    auto width = ToWidth(*type);
    auto isUnsigned = type->IsUnsignedInteger();
    if (operand.IsBottom()) {
        return SIntDomain::Bottom(width, isUnsigned);
    }
    if (operand.IsSingleValue()) {
        auto value = operand.NumericBound().GetSingleElement();
        if (isUnsigned) {
            uint64_t res = 0;
            auto isOverflow =
                OverflowChecker::IsUIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.UVal(), ov, &res);
            if (isOverflow && ov == OverflowStrategy::THROWING) {
                return SIntDomain::Top(width, true);
            }
            return {ConstantRange{SInt{width, res}}, true};
        }
        int64_t res = 0;
        auto isOverflow =
            OverflowChecker::IsIntOverflow(type->GetTypeKind(), ExprKind::NEG, 0, value.SVal(), ov, &res);
        if (isOverflow && ov == OverflowStrategy::THROWING) {
            return SIntDomain::Top(width, false);
        }
        return {ConstantRange{SInt{width, static_cast<uint64_t>(res)}}, false};
    }
    if (ov == OverflowStrategy::SATURATING || ov == OverflowStrategy::CHECKED) {
        return SIntDomain::Top(width, isUnsigned);
    }
    return {operand.NumericBound().Negate(), isUnsigned};
}

// 利用 ~x == -x - 1 计算按位取反值域。
SIntDomain ComputeBitNotRange(const SIntDomain& operand, bool isUnsigned)
{
    auto width = operand.Width();
    if (operand.IsBottom()) {
        return SIntDomain::Bottom(width, isUnsigned);
    }
    return {operand.NumericBound().Negate().Subtract(SInt{width, 1u}), isUnsigned};
}

// 从右操作数值域中提取合法常量移位量。
std::optional<unsigned> GetShiftAmount(const SIntDomain& range, bool isUnsigned, IntWidth lhsWidth)
{
    if (!range.IsSingleValue()) {
        return std::nullopt;
    }
    auto value = range.NumericBound().GetSingleElement();
    if (!isUnsigned && value.Slt(0)) {
        return std::nullopt;
    }
    auto amount = value.UVal();
    if (amount >= static_cast<unsigned>(lhsWidth)) {
        return std::nullopt;
    }
    return static_cast<unsigned>(amount);
}

// 对单点操作数精确计算位运算或移位结果。
SInt ApplyExactBitwise(ExprKind kind, const SInt& lhs, const SInt& rhs, bool isUnsigned)
{
    switch (kind) {
        case ExprKind::BITAND: {
            auto res = lhs;
            res &= rhs;
            return res;
        }
        case ExprKind::BITOR: {
            auto res = lhs;
            res |= rhs;
            return res;
        }
        case ExprKind::BITXOR: {
            auto res = lhs;
            res ^= rhs;
            return res;
        }
        case ExprKind::LSHIFT:
            return lhs.Shl(rhs);
        case ExprKind::RSHIFT:
            return isUnsigned ? lhs.LShr(rhs) : lhs.Ashr(rhs);
        default:
            CJC_ABORT();
            return lhs;
    }
}

// 尝试为常量移位量的移位表达式计算 sound 区间。
std::optional<SIntDomain> TryComputeShiftRange(
    ExprKind kind, const SIntDomain& lhs, const SIntDomain& rhs, bool rhsUnsigned, bool destUnsigned)
{
    auto width = lhs.Width();
    auto amount = GetShiftAmount(rhs, rhsUnsigned, width);
    if (!amount) {
        return SIntDomain::Top(width, destUnsigned);
    }
    if (lhs.IsBottom()) {
        return SIntDomain::Bottom(width, destUnsigned);
    }
    if (lhs.IsSingleValue()) {
        return SIntDomain{ConstantRange{ApplyExactBitwise(kind, lhs.NumericBound().GetSingleElement(),
                                          rhs.NumericBound().GetSingleElement(), destUnsigned)},
            destUnsigned};
    }
    if (*amount == 0U) {
        return SIntDomain{lhs.NumericBound(), destUnsigned};
    }

    auto& range = lhs.NumericBound();
    if (kind == ExprKind::LSHIFT) {
        if (destUnsigned) {
            if (range.IsWrappedSet()) {
                return std::nullopt;
            }
            auto min = range.UMinValue();
            auto max = range.UMaxValue();
            auto maxBeforeShift = SInt::UMaxValue(width).LShr(*amount);
            if (max.Ugt(maxBeforeShift)) {
                return std::nullopt;
            }
            return SIntDomain{RangeFromMinMax(min.Shl(*amount), max.Shl(*amount), true), true};
        }
        if (range.IsSignWrappedSet()) {
            return std::nullopt;
        }
        auto min = range.SMinValue();
        auto max = range.SMaxValue();
        auto maxBeforeShift = SInt::SMaxValue(width).LShr(*amount);
        if (min.Slt(0) || max.Sgt(maxBeforeShift)) {
            return std::nullopt;
        }
        return SIntDomain{RangeFromMinMax(min.Shl(*amount), max.Shl(*amount), false), false};
    }

    if (destUnsigned) {
        if (range.IsWrappedSet()) {
            return std::nullopt;
        }
        return SIntDomain{
            RangeFromMinMax(range.UMinValue().LShr(*amount), range.UMaxValue().LShr(*amount), true), true};
    }
    if (range.IsSignWrappedSet()) {
        return std::nullopt;
    }
    return SIntDomain{
        RangeFromMinMax(range.SMinValue().AShr(*amount), range.SMaxValue().AShr(*amount), false), false};
}

// 利用 mask 规则尝试收窄 x & mask 的值域。
std::optional<SIntDomain> TryComputeBitAndWithMask(const SIntDomain& value, const SInt& mask, bool destUnsigned)
{
    auto width = value.Width();
    if (mask.IsZero()) {
        return SIntDomain{ConstantRange{SInt::Zero(width)}, destUnsigned};
    }
    if (mask.IsAllOnes()) {
        return SIntDomain{value.NumericBound(), destUnsigned};
    }
    auto& range = value.NumericBound();
    if (destUnsigned && !range.IsWrappedSet() && (range.UMaxValue().UVal() & ~mask.UVal()) == 0U) {
        return SIntDomain{range, true};
    }
    if (destUnsigned || !mask.IsSignBitSet()) {
        return SIntDomain{RangeFromMinMax(SInt::Zero(width), mask, destUnsigned), destUnsigned};
    }
    return std::nullopt;
}

// 利用 mask 规则尝试收窄 x | mask 的值域。
std::optional<SIntDomain> TryComputeBitOrWithMask(const SIntDomain& value, const SInt& mask, bool destUnsigned)
{
    auto width = value.Width();
    if (mask.IsZero()) {
        return SIntDomain{value.NumericBound(), destUnsigned};
    }
    auto& range = value.NumericBound();
    if (destUnsigned && !range.IsWrappedSet() && !mask.IsSignBitSet() &&
        (range.UMaxValue().UVal() & mask.UVal()) == 0U) {
        auto min = range.UMinValue();
        min |= mask;
        auto max = range.UMaxValue();
        max |= mask;
        return SIntDomain{RangeFromMinMax(min, max, true), true};
    }
    if (destUnsigned) {
        return SIntDomain{RangeFromMinMax(mask, SInt::UMaxValue(width), true), true};
    }
    if (mask.IsSignBitSet()) {
        return SIntDomain{RangeFromMinMax(mask, SInt::AllOnes(width), false), false};
    }
    // 对已知非负的有符号值，保守推导 x | mask 的非负结果区间。
    if (!range.IsSignWrappedSet() && range.SMinValue().Sge(0)) {
        if ((range.SMaxValue().UVal() & mask.UVal()) == 0U) {
            auto min = range.SMinValue();
            min |= mask;
            auto max = range.SMaxValue();
            max |= mask;
            return SIntDomain{RangeFromMinMax(min, max, false), false};
        }
        return SIntDomain{RangeFromMinMax(mask, SInt::SMaxValue(width), false), false};
    }
    return std::nullopt;
}

// 尝试为位运算和移位表达式计算保守整数区间。
std::optional<SIntDomain> TryComputeBitwiseRange(ExprKind kind, const SIntDomain& lhs, const SIntDomain& rhs,
    Value* lhsValue, Value* rhsValue, bool rhsUnsigned, bool destUnsigned)
{
    auto width = lhs.Width();
    if (lhs.IsBottom() || rhs.IsBottom()) {
        return SIntDomain::Bottom(width, destUnsigned);
    }
    if (IsShiftBinaryExpr(kind)) {
        return TryComputeShiftRange(kind, lhs, rhs, rhsUnsigned, destUnsigned);
    }
    if (lhs.IsSingleValue() && rhs.IsSingleValue()) {
        return SIntDomain{ConstantRange{ApplyExactBitwise(
                              kind, lhs.NumericBound().GetSingleElement(), rhs.NumericBound().GetSingleElement(),
                              destUnsigned)},
            destUnsigned};
    }
    if (lhsValue == rhsValue) {
        if (kind == ExprKind::BITXOR) {
            return SIntDomain{ConstantRange{SInt::Zero(width)}, destUnsigned};
        }
        return SIntDomain{lhs.NumericBound(), destUnsigned};
    }
    if (rhs.IsSingleValue()) {
        auto mask = rhs.NumericBound().GetSingleElement();
        if (kind == ExprKind::BITAND) {
            return TryComputeBitAndWithMask(lhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITOR) {
            return TryComputeBitOrWithMask(lhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITXOR && mask.IsZero()) {
            return SIntDomain{lhs.NumericBound(), destUnsigned};
        }
    }
    if (lhs.IsSingleValue()) {
        auto mask = lhs.NumericBound().GetSingleElement();
        if (kind == ExprKind::BITAND) {
            return TryComputeBitAndWithMask(rhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITOR) {
            return TryComputeBitOrWithMask(rhs, mask, destUnsigned);
        }
        if (kind == ExprKind::BITXOR && mask.IsZero()) {
            return SIntDomain{rhs.NumericBound(), destUnsigned};
        }
    }
    return std::nullopt;
}

template <> const std::string Analysis<RangeDomain>::name = "range-analysis";
template <> const std::optional<unsigned> Analysis<RangeDomain>::blockLimit = 80;
template <> RangeDomain::ChildrenMap ValueAnalysis<RangeValueDomain>::globalChildrenMap{};
template <> RangeDomain::AllocatedRefMap ValueAnalysis<RangeValueDomain>::globalAllocatedRefMap{};
template <> RangeDomain::AllocatedObjMap ValueAnalysis<RangeValueDomain>::globalAllocatedObjMap{};
template <> std::vector<std::unique_ptr<Ref>> ValueAnalysis<RangeValueDomain>::globalRefPool{};
template <> std::vector<std::unique_ptr<AbstractObject>> ValueAnalysis<RangeValueDomain>::globalAbsObjPool{};
template <>
RangeDomain ValueAnalysis<RangeValueDomain>::globalState{&globalChildrenMap, &globalAllocatedRefMap,
    nullptr, &globalAllocatedObjMap, &globalRefPool, &globalAbsObjPool};

BoolDomain RangeAnalysis::GetBoolDomainFromState(const RangeDomain& state, const Ptr<Value>& value)
{
    if (!value->GetType()->IsBoolean()) {
        return BoolDomain::Top();
    }
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return BoolDomain::Top();
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::BOOL) {
        return BoolDomain::Top();
    }
    return StaticCast<const BoolRange*>(absVal)->GetVal();
}

// 从分析状态读取整数值域，未知时返回该类型的完整默认区间。
const SIntDomain& RangeAnalysis::GetSIntDomainFromState(const RangeDomain& state, const Ptr<Value>& value)
{
    CJC_ASSERT(value->GetType()->IsInteger());
    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (domain == nullptr || domain->IsTop()) {
        return GetDefaultIntCache(value->GetType());
    }
    auto absVal = domain->CheckAbsVal();
    if (absVal == nullptr || absVal->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return GetDefaultIntCache(value->GetType());
    }
    return StaticCast<const SIntRange*>(absVal)->GetVal();
}

// 根据普通表达式类别分派对应 transfer，并在调试模式下打印可见值域。
void RangeAnalysis::HandleNormalExpressionEffect(RangeDomain& state, const Expression* expression)
{
    switch (expression->GetExprMajorKind()) {
        case ExprMajorKind::MEMORY_EXPR:
            return;
        case ExprMajorKind::UNARY_EXPR:
            HandleUnaryExpr(state, StaticCast<const UnaryExpression*>(expression));
            break;
        case ExprMajorKind::BINARY_EXPR:
            HandleBinaryExpr(state, StaticCast<const BinaryExpression*>(expression));
            break;
        case ExprMajorKind::OTHERS:
            HandleOthersExpr(state, expression);
            break;
        case ExprMajorKind::STRUCTURED_CTRL_FLOW_EXPR:
        default: {
#ifndef NDEBUG
            CJC_ABORT();
#else
            return;
#endif
        }
    }
    if (expression->GetExprMajorKind() != ExprMajorKind::UNARY_EXPR &&
        expression->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR &&
        expression->GetExprKind() != ExprKind::TYPECAST) {
        return;
    }
    if (isDebug && !expression->GetResult()->GetType()->IsRef()) {
        if (expression->GetResult()->GetType()->IsBoolean()) {
            auto domain = GetBoolDomainFromState(state, expression->GetResult());
            if (!domain.IsTop()) {
                PrintDebugMessage<BoolDomain>(expression, domain);
            }
        } else if (expression->GetResult()->GetType()->IsInteger()) {
            auto domain = GetSIntDomainFromState(state, expression->GetResult());
            if (!domain.IsTop()) {
                PrintDebugMessage<SIntDomain>(expression, domain);
            }
        }
    }
}

// 由二元比较或布尔等值表达式生成 BoolDomain 结果。
BoolDomain RangeAnalysis::GenerateBoolRangeFromBinaryOp(
    RangeDomain& state, const Ptr<const BinaryExpression>& binaryExpr) const
{
    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    if (lhs->GetType()->IsInteger()) {
        const auto& lRange = GetSIntDomainFromState(state, lhs);
        const auto& rRange = GetSIntDomainFromState(state, rhs);
        return ComputeRelIntBinop(CHIRRelIntBinopArgs{
            lRange, rRange, lhs, rhs, binaryExpr->GetExprKind(), IsUnsignedArithmetic(*binaryExpr)});
    }
    const auto& lRange = GetBoolDomainFromState(state, lhs);
    const auto& rRange = GetBoolDomainFromState(state, rhs);
    return ComputeEqualityBoolBinop(lRange, rRange, binaryExpr->GetExprKind());
}

// 统计 block 入队次数，并在硬上限前应用循环 widening 快照。
bool RangeAnalysis::CheckInQueueTimes(const Block* block, RangeDomain& curState)
{
    if (inqueueTimes.count(block) == 0) {
        inqueueTimes[block] = 1;
        return false;
    }
    inqueueTimes[block]++;
    auto times = inqueueTimes.at(block);
    std::lock_guard<std::mutex> lock(loopRangeSnapshotsMtx);
    auto& snapshots = loopRangeSnapshots[this];
    if (times >= LOOP_WIDENING_START_TIMES) {
        auto previous = snapshots.find(block);
        if (previous != snapshots.end()) {
            ApplyLoopWidening(curState, previous->second, block);
        }
    }
    if (times >= LOOP_WIDENING_SNAPSHOT_START_TIMES) {
        snapshots[block] = CaptureLoopRangeSnapshot(curState, block);
    }
    if (times >= MAX_INQUEUE_TIMES) {
        curState.ClearState();
        return true;
    }
    return false;
}

// 处理一元布尔和整数表达式的值域转移。
void RangeAnalysis::HandleUnaryExpr(RangeDomain& state, const UnaryExpression* unaryExpr) const
{
    auto dest = unaryExpr->GetResult();
    if (unaryExpr->GetExprKind() == ExprKind::NOT && dest->GetType()->IsBoolean()) {
        auto operand = unaryExpr->GetOperand();
        auto operandRange = GetBoolDomainFromState(state, operand);
        if (operandRange.IsNonTrivial()) {
            return state.Update(dest, std::make_unique<BoolRange>(!operandRange));
        }
    }
    if (dest->GetType()->IsInteger()) {
        auto operand = unaryExpr->GetOperand();
        const auto& operandRange = GetSIntDomainFromState(state, operand);
        if (unaryExpr->GetExprKind() == ExprKind::NEG) {
            auto range = ComputeNegRange(operandRange, dest->GetType(), unaryExpr->GetOverflowStrategy());
            if (range.IsNonTrivial()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
            }
        } else if (unaryExpr->GetExprKind() == ExprKind::BITNOT) {
            auto range = ComputeBitNotRange(operandRange, dest->GetType()->IsUnsignedInteger());
            if (range.IsNonTrivial()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(range)));
            }
        }
    }
    return state.SetToBound(dest, true);
}

std::string GenerateTypeRangePrompt(const Ptr<Type>& type)
{
    const static std::unordered_map<Type::TypeKind, std::pair<int64_t, uint64_t>> TYPE_TO_RANGE = {
        {Type::TypeKind::TYPE_INT8, {std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()}},
        {Type::TypeKind::TYPE_INT16, {std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()}},
        {Type::TypeKind::TYPE_INT32, {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}},
        {Type::TypeKind::TYPE_INT64, {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}},
        {Type::TypeKind::TYPE_INT_NATIVE, {std::numeric_limits<ssize_t>::min(), std::numeric_limits<ssize_t>::max()}},
        {Type::TypeKind::TYPE_UINT8, {std::numeric_limits<uint8_t>::min(), std::numeric_limits<uint8_t>::max()}},
        {Type::TypeKind::TYPE_UINT16, {std::numeric_limits<uint16_t>::min(), std::numeric_limits<uint16_t>::max()}},
        {Type::TypeKind::TYPE_UINT32, {std::numeric_limits<uint32_t>::min(), std::numeric_limits<uint32_t>::max()}},
        {Type::TypeKind::TYPE_UINT64, {std::numeric_limits<uint64_t>::min(), std::numeric_limits<uint64_t>::max()}},
        {Type::TypeKind::TYPE_UINT_NATIVE, {std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max()}}};

    auto [min, max] = TYPE_TO_RANGE.at(type->GetTypeKind());
    return "range of " + type->ToString() + " is " + std::to_string(min) + " ~ " + std::to_string(max);
}

template <typename TBinary, typename T>
void RaiseArithmeticOverflowError(const TBinary* expr, ExprKind kind, T leftVal, T rightVal, DiagAdapter& diag)
{
    auto& loc = expr->GetDebugLocation();
    auto ty = expr->GetResult()->GetType();
    const static std::unordered_map<ExprKind, std::string> OPS = {
        {ExprKind::ADD, "+"},
        {ExprKind::SUB, "-"},
        {ExprKind::MUL, "*"},
        {ExprKind::DIV, "/"},
        {ExprKind::MOD, "%"},
        {ExprKind::EXP, "**"},
    };
    auto token = OPS.find(kind);
    CJC_ASSERT(token != OPS.end());
    auto builder =
        diag.DiagnoseRefactor(DiagKindRefactor::chir_arithmetic_operator_overflow, ToRange(loc), token->second);
    std::string hint = ty->ToString() + "(" + std::to_string(leftVal) + ") " + token->second + " " +
        expr->GetRHSOperand()->GetType()->ToString() + "(" + std::to_string(rightVal) + ")";
    builder.AddMainHintArguments(hint);
    builder.AddNote(GenerateTypeRangePrompt(expr->GetResult()->GetType()));
}

template <typename T>
bool CheckDivZero(ExprKind exprKind, const Ptr<const BinaryExpression>& binary, T rVal, DiagAdapter& diag)
{
    if (rVal == 0 && (exprKind == ExprKind::DIV || exprKind == ExprKind::MOD)) {
        auto& loc = binary->GetDebugLocation();
        auto prompt = exprKind == ExprKind::DIV ? "divide" : "modulo";
        auto builder = diag.DiagnoseRefactor(DiagKindRefactor::chir_divisor_is_zero, ToRange(loc), prompt);
        builder.AddMainHintArguments(prompt);
        return true;
    }
    return false;
}

// 对单点算术表达式执行精确计算，并处理除零和 throwing 溢出诊断。
SIntDomain CheckSingleValueOverflow(
    const CHIRArithmeticBinopArgs& args, const Ptr<const BinaryExpression>& expr, ExprKind exprKind, DiagAdapter& diag)
{
    bool isOv = false;
    if (args.uns) {
        uint64_t a = args.ld.NumericBound().Lower().UVal();
        uint64_t b = args.rd.NumericBound().Lower().UVal();
        uint64_t res = 0;
        if (CheckDivZero(exprKind, expr, b, diag)) {
            return SIntDomain::Top(args.ld.Width(), true);
        }
        isOv = OverflowChecker::IsUIntOverflow(args.l->GetType()->GetTypeKind(), exprKind, a, b, args.ov, &res);
        if (isOv && args.ov == OverflowStrategy::THROWING) {
            RaiseArithmeticOverflowError(expr.get(), exprKind, a, b, diag);
            return SIntDomain::Top(args.ld.Width(), true);
        }
        return {ConstantRange{{args.ld.Width(), res}}, true};
    } else {
        int64_t a = args.ld.NumericBound().Lower().SVal();
        int64_t b = args.rd.NumericBound().Lower().SVal();
        int64_t res = 0;
        if (CheckDivZero(exprKind, expr, b, diag)) {
            return SIntDomain::Top(args.ld.Width(), false);
        }
        isOv = OverflowChecker::IsIntOverflow(args.l->GetType()->GetTypeKind(), exprKind, a, b, args.ov, &res);
        if (isOv && args.ov == OverflowStrategy::THROWING) {
            RaiseArithmeticOverflowError(expr.get(), exprKind, a, b, diag);
            return SIntDomain::Top(args.ld.Width(), false);
        }
        return {ConstantRange{{args.ld.Width(), static_cast<uint64_t>(res)}}, false};
    }
}

// 处理算术、位运算、移位、关系和布尔二元表达式的值域转移。
void RangeAnalysis::HandleBinaryExpr(RangeDomain& state, const BinaryExpression* binaryExpr)
{
    auto dest = binaryExpr->GetResult();
    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    if (!CanAnalyse(dest->GetType()) || !CanAnalyse(lhs->GetType()) || !CanAnalyse(rhs->GetType())) {
        return state.SetToBound(binaryExpr->GetResult(), true);
    }
    if (dest->GetType()->IsInteger()) {
        auto kind = binaryExpr->GetExprKind();
        if (!IsBasicBinaryExpr(*binaryExpr) && !IsBitwiseBinaryExpr(kind) && !IsShiftBinaryExpr(kind)) {
            return state.SetToBound(binaryExpr->GetResult(), true);
        }
        if (auto inductionRange = TryComputeSimpleInductionUpdateRange(binaryExpr);
            inductionRange.has_value() && inductionRange->IsNonTrivial()) {
            return state.Update(dest, std::make_unique<SIntRange>(std::move(inductionRange.value())));
        }
        const auto& lRange = GetSIntDomainFromState(state, lhs);
        const auto& rRange = GetSIntDomainFromState(state, rhs);
        if (IsBitwiseBinaryExpr(kind) || IsShiftBinaryExpr(kind)) {
            auto res = TryComputeBitwiseRange(kind, lRange, rRange, lhs, rhs, rhs->GetType()->IsUnsignedInteger(),
                dest->GetType()->IsUnsignedInteger());
            if (res && res->IsNonTrivial()) {
                return state.Update(dest, std::make_unique<SIntRange>(std::move(*res)));
            }
            return state.SetToBound(binaryExpr->GetResult(), true);
        }
        auto ov = binaryExpr->GetOverflowStrategy();
        auto isUnsigned = IsUnsignedArithmetic(*binaryExpr);
        if (lRange.IsSingleValue() && rRange.IsSingleValue()) {
            auto domain = CheckSingleValueOverflow(
                CHIRArithmeticBinopArgs{lRange, rRange, lhs, rhs, kind, ov, isUnsigned}, binaryExpr, kind, *diag);
            state.Update(dest, std::make_unique<SIntRange>(domain));
            return;
        }
        auto res = ComputeArithmeticBinop(CHIRArithmeticBinopArgs{lRange, rRange, lhs, rhs, kind, ov, isUnsigned});
        if (res.IsNonTrivial()) {
            return state.Update(dest, std::make_unique<SIntRange>(res));
        }
    }
    if (dest->GetType()->IsBoolean()) {
        auto kind = binaryExpr->GetExprKind();
        auto res = IsLogicalBinaryExpr(kind)
            ? (kind == ExprKind::AND ? LogicalAnd(GetBoolDomainFromState(state, lhs), GetBoolDomainFromState(state, rhs))
                                      : LogicalOr(GetBoolDomainFromState(state, lhs), GetBoolDomainFromState(state, rhs)))
            : GenerateBoolRangeFromBinaryOp(state, binaryExpr);
        if (res.IsNonTrivial()) {
            return state.Update(dest, std::make_unique<BoolRange>(res));
        }
    }
    state.SetToBound(binaryExpr->GetResult(), true);
}

// 计算整数 typecast 后的值域，并在安全时保留非负符号约束。
SIntDomain RangeAnalysis::ComputeTypeCast(RangeDomain& state, PtrSymbol oldSymbol, const SIntDomain& v,
    IntWidth dstSize, bool dstUnsigned, OverflowStrategy ov) const
{
    auto numericRange{ComputeTypeCastNumericBound(v, dstSize, dstUnsigned, ov)};
    if (dstSize < v.Width() || v.IsUnsigned() || !dstUnsigned || ov == OverflowStrategy::SATURATING ||
        numericRange.SMinValue().Slt({dstSize, 0u})) {
        return {numericRange, v.IsUnsigned()};
    }
    // unsigned to signed, same width or larger width
    // in this special case, if we have a symbolic range a<b and we know definitely b>=0 && a>=0,
    // this range can be preserved
    SIntDomain::SymbolicBoundsMap mp{};
    for (auto it = v.SymbolicBounds().Begin(); it != v.SymbolicBounds().End(); it++) {
        auto absVal = state.CheckAbstractValue(it->first);
        if (absVal != nullptr && absVal->GetRangeKind() == ValueRange::RangeKind::SINT) {
            auto range = StaticCast<SIntRange>(absVal)->GetVal();
            if (range.NumericBound().SMinValue().Sge(SInt{range.Width(), 0u})) {
                mp.emplace(it->first,
                    NumericConversion(it->second, dstSize, false, false,
                        OverflowStrategy::THROWING)); // this typecast can never wrap, pass THROWING for better
                                                      // performance
            }
        }
    }
    mp.emplace(oldSymbol, ConstantRange{{dstSize, 0u}});
    return SIntDomain{numericRange, std::move(mp), v.IsUnsigned()};
}

// 处理 typecast 等其它表达式，并对未知结果设置保守 Top 或 TopRef。
void RangeAnalysis::HandleOthersExpr(RangeDomain& state, const Expression* expression)
{
    switch (expression->GetExprKind()) {
        case ExprKind::TYPECAST: {
            HandleTypeCast(state, StaticCast<const TypeCast*>(expression));
            break;
        }
        case ExprKind::CONSTANT:
        case ExprKind::APPLY:
        case ExprKind::FIELD:
            return;
        case ExprKind::TUPLE:
        default: {
            auto dest = expression->GetResult();
            return state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
        }
    }
}

namespace {
// 判断是否是能产生分支约束的比较表达式。
bool IsRelationalExprKind(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
        case ExprKind::LE:
        case ExprKind::GT:
        case ExprKind::GE:
        case ExprKind::EQUAL:
        case ExprKind::NOTEQUAL:
            return true;
        default:
            return false;
    }
}

// 将 CHIR 比较表达式类型转换为值域关系枚举。
RelationalOperation ToRelationalOperation(ExprKind kind)
{
    switch (kind) {
        case ExprKind::LT:
            return RelationalOperation::LT;
        case ExprKind::LE:
            return RelationalOperation::LE;
        case ExprKind::GT:
            return RelationalOperation::GT;
        case ExprKind::GE:
            return RelationalOperation::GE;
        case ExprKind::EQUAL:
            return RelationalOperation::EQ;
        case ExprKind::NOTEQUAL:
            return RelationalOperation::NE;
        default:
            CJC_ABORT();
            return RelationalOperation::NE;
    }
}

// 获取关系的逻辑取反形式，用于 false 边。
RelationalOperation NegateRelation(RelationalOperation rel)
{
    switch (rel) {
        case RelationalOperation::LT:
            return RelationalOperation::GE;
        case RelationalOperation::LE:
            return RelationalOperation::GT;
        case RelationalOperation::GT:
            return RelationalOperation::LE;
        case RelationalOperation::GE:
            return RelationalOperation::LT;
        case RelationalOperation::EQ:
            return RelationalOperation::NE;
        case RelationalOperation::NE:
            return RelationalOperation::EQ;
    }
}

// 交换左右操作数时同步转换关系方向。
RelationalOperation SwapRelation(RelationalOperation rel)
{
    switch (rel) {
        case RelationalOperation::LT:
            return RelationalOperation::GT;
        case RelationalOperation::LE:
            return RelationalOperation::GE;
        case RelationalOperation::GT:
            return RelationalOperation::LT;
        case RelationalOperation::GE:
            return RelationalOperation::LE;
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return rel;
    }
}

bool CanReachBlock(const Block* start, const Block* target, std::unordered_set<const Block*>& visited);
bool IsLoopBranch(const Branch* branch);
bool NarrowSIntByRelationToConstant(RangeDomain& state, Value* value, RelationalOperation rel, const SInt& constant);
std::optional<int64_t> GetUpdateStepFromLocation(Value* value, Value* location);
bool ApplyConditionConstraint(RangeDomain& state, Value* condition, bool branchCondition);

// 获取局部 SSA 值的定义表达式。
const Expression* GetDefiningExpr(Value* value)
{
    if (value == nullptr || !value->IsLocalVar()) {
        return nullptr;
    }
    return StaticCast<LocalVar*>(value)->GetExpr();
}

// 判断值是否可被 RangeDomain 状态跟踪。
bool IsStateTrackedValue(Value* value)
{
    return value != nullptr && (value->IsLocalVar() || value->IsParameter());
}

// 判断值是否是可跟踪的整数值。
bool IsIntegerValue(Value* value)
{
    return IsStateTrackedValue(value) && value->GetType()->IsInteger();
}

// 判断值是否是可跟踪的布尔值。
bool IsBooleanValue(Value* value)
{
    return IsStateTrackedValue(value) && value->GetType()->IsBoolean();
}

// 将布尔值收窄到分支期望的真假值。
bool NarrowBoolValue(RangeDomain& state, Value* value, bool expected)
{
    if (!IsBooleanValue(value)) {
        return true;
    }
    auto current = RangeAnalysis::GetBoolDomainFromState(state, value);
    if (current.IsSingleValue() && current.GetSingleValue() != expected) {
        state.Update(value, std::make_unique<BoolRange>(BoolDomain::Bottom()));
        return true;
    }
    state.Update(value, std::make_unique<BoolRange>(BoolDomain::FromBool(expected)));
    return true;
}

// 对两个整数域求交，必要时用 unsigned 语义重试。
SIntDomain IntersectForNarrowing(const SIntDomain& current, const SIntDomain& constraint)
{
    auto narrowed = SIntDomain::Intersects(current, constraint);
    if (!narrowed.IsBottom() || !current.IsUnsigned() || !constraint.IsUnsigned() ||
        current.Width() != constraint.Width()) {
        return narrowed;
    }

    auto unsignedNumeric =
        current.NumericBound().IntersectWith(constraint.NumericBound(), PreferredRangeType::Unsigned);
    if (unsignedNumeric.IsEmptySet()) {
        return SIntDomain::Bottom(current.Width(), current.IsUnsigned());
    }
    return SIntDomain{unsignedNumeric, current.IsUnsigned()};
}

// 将可跟踪整数值与约束域求交以完成收窄。
bool NarrowSIntValue(RangeDomain& state, Value* value, const SIntDomain& constraint)
{
    if (!IsIntegerValue(value)) {
        return true;
    }
    const auto& current = RangeAnalysis::GetSIntDomainFromState(state, value);
    auto narrowed = IntersectForNarrowing(current, constraint);
    if (narrowed.IsBottom()) {
        auto type = value->GetType();
        state.Update(value,
            std::make_unique<SIntRange>(SIntDomain::Bottom(ToWidth(*type), type->IsUnsignedInteger())));
        return true;
    }
    state.Update(value, std::make_unique<SIntRange>(std::move(narrowed)));
    return true;
}

// 将 load 的收窄结果反向传播到其引用的整数对象。
bool NarrowLoadedSIntLocation(RangeDomain& state, Value* value, const SIntDomain& constraint)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return true;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    if (location == nullptr || !location->GetType()->IsRef()) {
        return true;
    }
    auto refType = StaticCast<RefType*>(location->GetType());
    auto rootType = refType->GetRootBaseType();
    if (rootType == nullptr || !rootType->IsInteger() || ToWidth(*rootType) != constraint.Width()) {
        return true;
    }
    auto object = state.CheckAbstractObjectRefBy(location);
    if (object == nullptr) {
        return true;
    }
    const auto& current = GetSIntDomainFromState(state, object, rootType);
    auto narrowed = IntersectForNarrowing(current, constraint);
    if (narrowed.IsBottom()) {
        state.Update(object,
            std::make_unique<SIntRange>(SIntDomain::Bottom(ToWidth(*rootType), rootType->IsUnsignedInteger())));
        return true;
    }
    state.Update(object, std::make_unique<SIntRange>(std::move(narrowed)));
    return true;
}

// 按照与常量的关系收窄整数值。
bool NarrowSIntByRelationToConstant(RangeDomain& state, Value* value, RelationalOperation rel, const SInt& constant)
{
    auto type = value->GetType();
    auto constraint = SIntDomain::FromNumeric(rel, constant, type->IsUnsignedInteger());
    if (!NarrowSIntValue(state, value, constraint)) {
        return false;
    }
    if (rel != RelationalOperation::NE) {
        return NarrowLoadedSIntLocation(state, value, constraint);
    }
    return true;
}

// 从定义常量中读取单点整数值。
std::optional<SInt> GetSingleIntFromDefiningConstant(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::CONSTANT) {
        return std::nullopt;
    }
    auto literal = StaticCast<const Constant*>(expr)->GetValue();
    if (literal == nullptr || !literal->IsIntLiteral()) {
        return std::nullopt;
    }
    auto domain = SIntDomain::From(*literal);
    if (!domain.IsSingleValue()) {
        return std::nullopt;
    }
    return domain.NumericBound().GetSingleElement();
}

// 从定义常量中读取单点布尔值。
std::optional<bool> GetSingleBoolFromDefiningConstant(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::CONSTANT) {
        return std::nullopt;
    }
    auto literal = StaticCast<const Constant*>(expr)->GetValue();
    if (literal == nullptr || !literal->IsBoolLiteral()) {
        return std::nullopt;
    }
    return StaticCast<BoolLiteral*>(literal)->GetVal();
}

// 优先从状态读取单点布尔值，失败时回退到定义常量。
std::optional<bool> GetSingleBoolFromStateOrConstant(const RangeDomain& state, Value* value)
{
    auto domain = RangeAnalysis::GetBoolDomainFromState(state, value);
    if (domain.IsSingleValue()) {
        return domain.GetSingleValue();
    }
    return GetSingleBoolFromDefiningConstant(value);
}

// 判断某个值是否是从指定 ref 位置 load 出来的。
bool IsLoadFromLocation(Value* value, Value* location)
{
    auto expr = GetDefiningExpr(value);
    return expr != nullptr && expr->GetExprKind() == ExprKind::LOAD &&
        StaticCast<const Load*>(expr)->GetLocation() == location;
}

// 读取可用于单调更新判断的非负整数常量。
std::optional<SInt> GetNonNegativeDefiningConstant(Value* value)
{
    auto constant = GetSingleIntFromDefiningConstant(value);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    if (value->GetType()->IsUnsignedInteger() || constant->IsNonNeg()) {
        return constant;
    }
    return std::nullopt;
}

// 判断存储值是否来自同一位置加非负常量。
bool IsNonDecreasingValueFromLocation(Value* value, Value* location)
{
    if (IsLoadFromLocation(value, location)) {
        return true;
    }
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::ADD) {
        return false;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    return (IsLoadFromLocation(lhs, location) && GetNonNegativeDefiningConstant(rhs).has_value()) ||
        (IsLoadFromLocation(rhs, location) && GetNonNegativeDefiningConstant(lhs).has_value());
}

// 判断前驱是否能回到 header，从而构成回边。
// 在不穿过 header 的前提下检查可达性，用于区分内层循环 preheader 和真正回边。
bool CanReachBlockAvoidingHeader(
    const Block* start, const Block* target, const Block* header, std::unordered_set<const Block*>& visited)
{
    if (start == nullptr || target == nullptr || start == header || !visited.emplace(start).second) {
        return false;
    }
    if (start == target) {
        return true;
    }
    for (auto successor : start->GetSuccessors()) {
        if (CanReachBlockAvoidingHeader(successor, target, header, visited)) {
            return true;
        }
    }
    return false;
}

// 判断 header 的某个后继是否是能回到 header 的循环体入口。
bool IsLoopBodySuccessor(const Branch* branch, const Block* successor)
{
    auto header = branch == nullptr ? nullptr : branch->GetParentBlock();
    if (branch != nullptr &&
        (branch->GetSourceExpr() == SourceExpr::WHILE_EXPR || branch->GetSourceExpr() == SourceExpr::DO_WHILE_EXPR)) {
        return successor == branch->GetTrueBlock();
    }
    std::unordered_set<const Block*> visited;
    return CanReachBlock(successor, header, visited);
}

bool IsBackedgePredecessor(const Block* header, const Block* pred)
{
    auto terminator = header == nullptr ? nullptr : header->GetTerminator();
    if (terminator != nullptr && terminator->GetExprKind() == ExprKind::BRANCH) {
        auto branch = StaticCast<const Branch*>(terminator);
        bool hasLoopBodySuccessor = false;
        std::vector<const Block*> successors{branch->GetTrueBlock(), branch->GetFalseBlock()};
        for (auto successor : successors) {
            if (!IsLoopBodySuccessor(branch, successor)) {
                continue;
            }
            hasLoopBodySuccessor = true;
            std::unordered_set<const Block*> visited;
            if (CanReachBlockAvoidingHeader(successor, pred, header, visited)) {
                return true;
            }
        }
        if (hasLoopBodySuccessor) {
            return false;
        }
    }
    std::unordered_set<const Block*> visited;
    return CanReachBlock(header, pred, visited);
}

// 检查所有回边 store 是否都是非递减更新。
bool HasNonDecreasingBackedgeStore(const Block* header, Value* location)
{
    bool hasBackedge = false;
    for (auto pred : header->GetPredecessors()) {
        if (!IsBackedgePredecessor(header, pred)) {
            continue;
        }
        hasBackedge = true;
        for (auto expr : pred->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() == location && !IsNonDecreasingValueFromLocation(store->GetValue(), location)) {
                return false;
            }
        }
    }
    return hasBackedge;
}

// 查找循环入口边对循环携带位置写入的唯一常量。
std::optional<SInt> FindIncomingStoreConstant(const Block* header, Value* location)
{
    if (!HasNonDecreasingBackedgeStore(header, location)) {
        return std::nullopt;
    }
    std::optional<SInt> incoming;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        auto exprs = pred->GetExpressions();
        for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
            if ((*it)->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(*it);
            if (store->GetLocation() != location) {
                continue;
            }
            auto constant = GetSingleIntFromDefiningConstant(store->GetValue());
            if (!constant.has_value()) {
                return std::nullopt;
            }
            if (incoming.has_value() && incoming.value() != constant.value()) {
                return std::nullopt;
            }
            incoming = constant.value();
            break;
        }
    }
    return incoming;
}

// 将循环入口常量恢复为递增归纳 load 的下界。
bool RestoreLoopIncomingLowerBound(RangeDomain& state, Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return true;
    }
    auto load = StaticCast<const Load*>(expr);
    auto header = load->GetParentBlock();
    auto incoming = FindIncomingStoreConstant(header, load->GetLocation());
    if (!incoming.has_value()) {
        return true;
    }
    return NarrowSIntByRelationToConstant(state, value, RelationalOperation::GE, incoming.value());
}

// 判断关系是否为左操作数提供上界。
bool HasUpperBoundRelation(RelationalOperation rel)
{
    return rel == RelationalOperation::LT || rel == RelationalOperation::LE || rel == RelationalOperation::EQ;
}

// 判断关系是否为左操作数提供下界。
bool HasLowerBoundRelation(RelationalOperation rel)
{
    return rel == RelationalOperation::GT || rel == RelationalOperation::GE || rel == RelationalOperation::EQ;
}

// 从定义常量中读取有符号整数值。
std::optional<int64_t> GetSignedConstantFromDefiningConstant(Value* value)
{
    if (value == nullptr || !value->GetType()->IsInteger() || value->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }
    auto constant = GetSingleIntFromDefiningConstant(value);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    return constant->SVal();
}

// 如果值是 load 表达式，返回其 ref 位置。
Value* GetLoadLocation(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return nullptr;
    }
    return StaticCast<const Load*>(expr)->GetLocation();
}

// 对有符号 step 取负，并排除 int64 最小值溢出。
std::optional<int64_t> NegateSignedStep(int64_t step)
{
    if (step == std::numeric_limits<int64_t>::min()) {
        return std::nullopt;
    }
    return -step;
}

// 提取循环携带位置更新时使用的常量步长。
std::optional<int64_t> GetUpdateStepFromLocation(Value* value, Value* location)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr) {
        return std::nullopt;
    }
    if (IsLoadFromLocation(value, location)) {
        return 0;
    }
    if (expr->GetExprKind() != ExprKind::ADD && expr->GetExprKind() != ExprKind::SUB) {
        return std::nullopt;
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (expr->GetExprKind() == ExprKind::ADD) {
        if (IsLoadFromLocation(lhs, location)) {
            return GetSignedConstantFromDefiningConstant(rhs);
        }
        if (IsLoadFromLocation(rhs, location)) {
            return GetSignedConstantFromDefiningConstant(lhs);
        }
        return std::nullopt;
    }
    if (!IsLoadFromLocation(lhs, location)) {
        return std::nullopt;
    }
    auto rhsStep = GetSignedConstantFromDefiningConstant(rhs);
    if (!rhsStep.has_value()) {
        return std::nullopt;
    }
    return NegateSignedStep(rhsStep.value());
}

// 查找循环回边上唯一的常量更新步长。
std::optional<int64_t> FindSingleBackedgeStep(const Block* header, Value* location)
{
    std::optional<int64_t> step;
    bool hasBackedge = false;
    for (auto pred : header->GetPredecessors()) {
        if (!IsBackedgePredecessor(header, pred)) {
            continue;
        }
        hasBackedge = true;
        std::optional<int64_t> predStep;
        for (auto expr : pred->GetExpressions()) {
            if (expr->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(expr);
            if (store->GetLocation() != location) {
                continue;
            }
            if (predStep.has_value()) {
                return std::nullopt;
            }
            predStep = GetUpdateStepFromLocation(store->GetValue(), location);
            if (!predStep.has_value()) {
                return std::nullopt;
            }
        }
        if (!predStep.has_value()) {
            return std::nullopt;
        }
        if (step.has_value() && step.value() != predStep.value()) {
            return std::nullopt;
        }
        step = predStep.value();
    }
    if (!hasBackedge) {
        return std::nullopt;
    }
    return step;
}

// 查找循环入口边写入的唯一有符号常量。
std::optional<int64_t> FindIncomingSignedStoreConstant(const Block* header, Value* location)
{
    std::optional<int64_t> incoming;
    bool hasIncoming = false;
    for (auto pred : header->GetPredecessors()) {
        if (IsBackedgePredecessor(header, pred)) {
            continue;
        }
        hasIncoming = true;
        std::optional<int64_t> predIncoming;
        auto exprs = pred->GetExpressions();
        for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
            if ((*it)->GetExprKind() != ExprKind::STORE) {
                continue;
            }
            auto store = StaticCast<const Store*>(*it);
            if (store->GetLocation() != location) {
                continue;
            }
            predIncoming = GetSignedConstantFromDefiningConstant(store->GetValue());
            break;
        }
        if (!predIncoming.has_value()) {
            return std::nullopt;
        }
        if (incoming.has_value() && incoming.value() != predIncoming.value()) {
            return std::nullopt;
        }
        incoming = predIncoming.value();
    }
    if (!hasIncoming) {
        return std::nullopt;
    }
    return incoming;
}

// 将循环入口常量恢复为递减归纳 load 的上界。
bool RestoreLoopIncomingUpperBound(RangeDomain& state, Value* value)
{
    auto location = GetLoadLocation(value);
    if (location == nullptr) {
        return true;
    }
    auto header = StaticCast<const Load*>(GetDefiningExpr(value))->GetParentBlock();
    auto init = FindIncomingSignedStoreConstant(header, location);
    auto step = FindSingleBackedgeStep(header, location);
    if (!init.has_value() || !step.has_value() || step.value() >= 0) {
        return true;
    }
    auto width = ToWidth(*value->GetType());
    return NarrowSIntByRelationToConstant(
        state, value, RelationalOperation::LE, SInt{width, static_cast<uint64_t>(init.value())});
}

struct SimpleInductionCondition {
    Value* loadValue;
    Value* location;
    RelationalOperation relation;
    int64_t bound;
};

// 识别 load(location) 与有符号常量比较形成的循环 guard。
std::optional<SimpleInductionCondition> GetSimpleInductionCondition(Value* condition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || !IsRelationalExprKind(expr->GetExprKind())) {
        return std::nullopt;
    }
    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (!lhs->GetType()->IsInteger() || !rhs->GetType()->IsInteger() || lhs->GetType()->IsUnsignedInteger() ||
        rhs->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }

    auto rel = ToRelationalOperation(expr->GetExprKind());
    if (auto lhsLocation = GetLoadLocation(lhs); lhsLocation != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(rhs);
        if (!bound.has_value()) {
            return std::nullopt;
        }
        return SimpleInductionCondition{lhs, lhsLocation, rel, bound.value()};
    }
    if (auto rhsLocation = GetLoadLocation(rhs); rhsLocation != nullptr) {
        auto bound = GetSignedConstantFromDefiningConstant(lhs);
        if (!bound.has_value()) {
            return std::nullopt;
        }
        return SimpleInductionCondition{rhs, rhsLocation, SwapRelation(rel), bound.value()};
    }
    return std::nullopt;
}

// 返回指定 CHIR 整数宽度下的有符号最小值和最大值。
std::pair<int64_t, int64_t> SignedLimits(IntWidth width)
{
    if (width == IntWidth::I64) {
        return {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    }
    auto bits = static_cast<unsigned>(width);
    int64_t max = (1LL << (bits - 1U)) - 1;
    int64_t min = -(1LL << (bits - 1U));
    return {min, max};
}

// 判断扩展精度计算结果是否仍适配目标有符号宽度。
bool FitsSignedWidth(__int128 value, IntWidth width)
{
    auto [min, max] = SignedLimits(width);
    return value >= static_cast<__int128>(min) && value <= static_cast<__int128>(max);
}

// 计算正数归纳变量推导中使用的向上整除。
__int128 CeilDivPositive(__int128 numerator, __int128 denominator)
{
    CJC_ASSERT(numerator > 0 && denominator > 0);
    return (numerator + denominator - 1) / denominator;
}

// 计算可证明常量步长归纳变量的精确循环退出值。
std::optional<SInt> ComputeExactInductionExit(
    int64_t init, int64_t step, RelationalOperation relation, int64_t bound, IntWidth width)
{
    if (step == 0) {
        return std::nullopt;
    }
    __int128 exact = init;
    if (step > 0) {
        __int128 threshold = bound;
        if (relation == RelationalOperation::LE) {
            if (bound == std::numeric_limits<int64_t>::max()) {
                return std::nullopt;
            }
            threshold = static_cast<__int128>(bound) + 1;
        } else if (relation != RelationalOperation::LT) {
            return std::nullopt;
        }
        if (static_cast<__int128>(init) < threshold) {
            exact = static_cast<__int128>(init) +
                CeilDivPositive(threshold - static_cast<__int128>(init), step) * step;
        }
    } else {
        if (step == std::numeric_limits<int64_t>::min()) {
            return std::nullopt;
        }
        auto absStep = -static_cast<__int128>(step);
        __int128 threshold = bound;
        if (relation == RelationalOperation::GE) {
            if (bound == std::numeric_limits<int64_t>::min()) {
                return std::nullopt;
            }
            threshold = static_cast<__int128>(bound) - 1;
        } else if (relation != RelationalOperation::GT) {
            return std::nullopt;
        }
        if (static_cast<__int128>(init) > threshold) {
            exact = static_cast<__int128>(init) -
                CeilDivPositive(static_cast<__int128>(init) - threshold, absStep) * absStep;
        }
    }
    if (!FitsSignedWidth(exact, width)) {
        return std::nullopt;
    }
    return SInt{width, static_cast<uint64_t>(static_cast<int64_t>(exact))};
}

// 判断分支后继是否会离开该分支块所在循环。
bool IsLoopExitSuccessor(const Branch* branch, const Block* successor)
{
    if (branch != nullptr &&
        (branch->GetSourceExpr() == SourceExpr::WHILE_EXPR || branch->GetSourceExpr() == SourceExpr::DO_WHILE_EXPR)) {
        return successor == branch->GetFalseBlock();
    }
    if (!IsLoopBranch(branch)) {
        return false;
    }
    std::unordered_set<const Block*> visited;
    return !CanReachBlock(successor, branch->GetParentBlock(), visited);
}

// 在识别出简单归纳模式时将循环退出状态收窄为精确值。
bool TryNarrowSimpleInductionExit(RangeDomain& state, const Branch* branch, const Block* successor)
{
    if (!IsLoopExitSuccessor(branch, successor)) {
        return true;
    }
    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value()) {
        return true;
    }
    auto loadType = condition->loadValue->GetType();
    if (loadType->IsUnsignedInteger()) {
        return true;
    }
    auto init = FindIncomingSignedStoreConstant(branch->GetParentBlock(), condition->location);
    auto step = FindSingleBackedgeStep(branch->GetParentBlock(), condition->location);
    if (!init.has_value() || !step.has_value()) {
        return true;
    }
    auto exact = ComputeExactInductionExit(
        init.value(), step.value(), condition->relation, condition->bound, ToWidth(*loadType));
    if (!exact.has_value()) {
        return true;
    }
    NarrowSIntByRelationToConstant(state, condition->loadValue, RelationalOperation::EQ, exact.value());
    if (auto object = state.CheckAbstractObjectRefBy(condition->location); object != nullptr) {
        state.Update(object,
            std::make_unique<SIntRange>(SIntDomain::FromNumeric(
                RelationalOperation::EQ, exact.value(), loadType->IsUnsignedInteger())));
    }
    return true;
}

// 获取同宽整数 typecast 的源值，用于回推 case 约束。
Value* GetSameWidthTypeCastSource(Value* value)
{
    auto expr = GetDefiningExpr(value);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::TYPECAST) {
        return nullptr;
    }
    auto source = StaticCast<const TypeCast*>(expr)->GetSourceValue();
    if (!IsIntegerValue(source) || ToWidth(*source->GetType()) != ToWidth(*value->GetType())) {
        return nullptr;
    }
    return source;
}

// 使用单个 case 常量收窄 MultiBranch 目标值。
bool NarrowMultiBranchTarget(RangeDomain& state, Value* value, RelationalOperation rel, uint64_t caseVal)
{
    if (!IsIntegerValue(value)) {
        return true;
    }
    auto width = ToWidth(*value->GetType());
    return NarrowSIntValue(
        state, value, SIntDomain::FromNumeric(rel, SInt{width, caseVal}, value->GetType()->IsUnsignedInteger()));
}

// 收窄 MultiBranch 条件，并在存在同宽 typecast 时回推源值。
bool NarrowMultiBranchCondition(RangeDomain& state, Value* cond, RelationalOperation rel, uint64_t caseVal)
{
    if (!NarrowMultiBranchTarget(state, cond, rel, caseVal)) {
        return false;
    }
    auto source = GetSameWidthTypeCastSource(cond);
    if (source != nullptr && source != cond) {
        return NarrowMultiBranchTarget(state, source, rel, caseVal);
    }
    return true;
}

// 将整数比较约束应用到分支边状态。
bool ApplyIntComparisonConstraint(
    RangeDomain& state, Value* lhs, Value* rhs, RelationalOperation rel, bool branchCondition)
{
    if (!IsIntegerValue(lhs) || !IsIntegerValue(rhs)) {
        return true;
    }
    if (!branchCondition) {
        rel = NegateRelation(rel);
    }

    const auto& lhsDomain = RangeAnalysis::GetSIntDomainFromState(state, lhs);
    const auto& rhsDomain = RangeAnalysis::GetSIntDomainFromState(state, rhs);
    if (rhsDomain.IsSingleValue()) {
        if (!NarrowSIntByRelationToConstant(state, lhs, rel, rhsDomain.NumericBound().GetSingleElement())) {
            return false;
        }
    } else if (auto rhsConstant = GetSingleIntFromDefiningConstant(rhs)) {
        if (!NarrowSIntByRelationToConstant(state, lhs, rel, rhsConstant.value())) {
            return false;
        }
    }
    if (lhsDomain.IsSingleValue()) {
        if (!NarrowSIntByRelationToConstant(state, rhs, SwapRelation(rel), lhsDomain.NumericBound().GetSingleElement())) {
            return false;
        }
    } else if (auto lhsConstant = GetSingleIntFromDefiningConstant(lhs)) {
        if (!NarrowSIntByRelationToConstant(state, rhs, SwapRelation(rel), lhsConstant.value())) {
            return false;
        }
    }
    if (lhs == rhs || ToWidth(*lhs->GetType()) != ToWidth(*rhs->GetType())) {
        return true;
    }
    if (!NarrowSIntValue(state, lhs,
        SIntDomain::FromSymbolic(rel, rhs, ToWidth(*lhs->GetType()), lhs->GetType()->IsUnsignedInteger()))) {
        return false;
    }
    if (!NarrowSIntValue(state, rhs,
        SIntDomain::FromSymbolic(SwapRelation(rel), lhs, ToWidth(*rhs->GetType()), rhs->GetType()->IsUnsignedInteger()))) {
        return false;
    }
    if (HasUpperBoundRelation(rel) && !RestoreLoopIncomingLowerBound(state, lhs)) {
        return false;
    }
    if (HasUpperBoundRelation(SwapRelation(rel)) && !RestoreLoopIncomingLowerBound(state, rhs)) {
        return false;
    }
    if (HasLowerBoundRelation(rel) && !RestoreLoopIncomingUpperBound(state, lhs)) {
        return false;
    }
    if (HasLowerBoundRelation(SwapRelation(rel)) && !RestoreLoopIncomingUpperBound(state, rhs)) {
        return false;
    }
    return true;
}

// 将布尔相等或不等约束应用到分支边状态。
bool ApplyBoolEqualityConstraint(RangeDomain& state, Value* lhs, Value* rhs, RelationalOperation rel, bool branchCondition)
{
    if (!IsBooleanValue(lhs) || !IsBooleanValue(rhs)) {
        return true;
    }
    if (!branchCondition) {
        rel = NegateRelation(rel);
    }
    if (rel != RelationalOperation::EQ && rel != RelationalOperation::NE) {
        return true;
    }

    auto lhsValue = GetSingleBoolFromStateOrConstant(state, lhs);
    auto rhsValue = GetSingleBoolFromStateOrConstant(state, rhs);
    if (lhsValue.has_value()) {
        auto value = lhsValue.value();
        if (!NarrowBoolValue(state, rhs, rel == RelationalOperation::EQ ? value : !value)) {
            return false;
        }
    }
    if (rhsValue.has_value()) {
        auto value = rhsValue.value();
        if (!NarrowBoolValue(state, lhs, rel == RelationalOperation::EQ ? value : !value)) {
            return false;
        }
    }
    return true;
}

// 处理 && 和 || 条件中的布尔恒等式与短路约束。
bool ApplyLogicalBoolConstraint(
    RangeDomain& state, const BinaryExpression* binary, ExprKind kind, bool branchCondition)
{
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    if (!IsBooleanValue(lhs) || !IsBooleanValue(rhs)) {
        return true;
    }

    auto lhsValue = GetSingleBoolFromStateOrConstant(state, lhs);
    auto rhsValue = GetSingleBoolFromStateOrConstant(state, rhs);
    if (kind == ExprKind::OR) {
        if (lhsValue.has_value()) {
            if (lhsValue.value()) {
                return branchCondition;
            }
            return ApplyConditionConstraint(state, rhs, branchCondition);
        }
        if (rhsValue.has_value()) {
            if (rhsValue.value()) {
                return branchCondition;
            }
            return ApplyConditionConstraint(state, lhs, branchCondition);
        }
        if (!branchCondition) {
            return ApplyConditionConstraint(state, lhs, false) && ApplyConditionConstraint(state, rhs, false);
        }
        return true;
    }

    if (kind == ExprKind::AND) {
        if (lhsValue.has_value()) {
            if (!lhsValue.value()) {
                return !branchCondition;
            }
            return ApplyConditionConstraint(state, rhs, branchCondition);
        }
        if (rhsValue.has_value()) {
            if (!rhsValue.value()) {
                return !branchCondition;
            }
            return ApplyConditionConstraint(state, lhs, branchCondition);
        }
        if (branchCondition) {
            return ApplyConditionConstraint(state, lhs, true) && ApplyConditionConstraint(state, rhs, true);
        }
    }
    return true;
}

// 在一个 block 中查找最后一次写入 ref 位置的布尔常量。
std::optional<bool> FindStoredBoolConstant(const Block* block, Value* location)
{
    auto exprs = block->GetExpressions();
    for (auto it = exprs.rbegin(); it != exprs.rend(); ++it) {
        if ((*it)->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        auto store = StaticCast<const Store*>(*it);
        if (store->GetLocation() != location) {
            continue;
        }
        return GetSingleBoolFromDefiningConstant(store->GetValue());
    }
    return std::nullopt;
}

struct ShortCircuitBoolAlias {
    Value* source;
    bool inverted;
};

// 从 lowered 短路布尔临时 load 中恢复原始 flag。
std::optional<ShortCircuitBoolAlias> GetShortCircuitBoolAlias(Value* condition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr || expr->GetExprKind() != ExprKind::LOAD) {
        return std::nullopt;
    }
    auto location = StaticCast<const Load*>(expr)->GetLocation();
    auto loadBlock = expr->GetParentBlock();
    if (location == nullptr || loadBlock == nullptr) {
        return std::nullopt;
    }
    auto preds = loadBlock->GetPredecessors();
    if (preds.size() != 2) {
        return std::nullopt;
    }

    const Block* controller = nullptr;
    for (auto pred : preds) {
        auto predPreds = pred->GetPredecessors();
        if (predPreds.size() != 1) {
            return std::nullopt;
        }
        if (controller == nullptr) {
            controller = predPreds.front();
        } else if (controller != predPreds.front()) {
            return std::nullopt;
        }
    }
    auto terminator = controller == nullptr ? nullptr : controller->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return std::nullopt;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    auto trueBlock = branch->GetTrueBlock();
    auto falseBlock = branch->GetFalseBlock();
    if ((trueBlock != preds[0] && trueBlock != preds[1]) || (falseBlock != preds[0] && falseBlock != preds[1])) {
        return std::nullopt;
    }
    auto trueValue = FindStoredBoolConstant(trueBlock, location);
    auto falseValue = FindStoredBoolConstant(falseBlock, location);
    if (!trueValue.has_value() || !falseValue.has_value() || trueValue.value() == falseValue.value()) {
        return std::nullopt;
    }
    auto source = branch->GetCondition();
    if (source == condition || !IsBooleanValue(source)) {
        return std::nullopt;
    }
    return ShortCircuitBoolAlias{source, !trueValue.value() && falseValue.value()};
}

// 将所有支持的分支条件约束应用到出边状态。
bool ApplyConditionConstraint(RangeDomain& state, Value* condition, bool branchCondition)
{
    auto expr = GetDefiningExpr(condition);
    if (expr == nullptr) {
        return NarrowBoolValue(state, condition, branchCondition);
    }
    if (auto alias = GetShortCircuitBoolAlias(condition); alias.has_value()) {
        if (!ApplyConditionConstraint(state, alias->source, alias->inverted ? !branchCondition : branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if (expr->GetExprKind() == ExprKind::NOT) {
        if (!ApplyConditionConstraint(state, StaticCast<const UnaryExpression*>(expr)->GetOperand(), !branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if ((expr->GetExprKind() == ExprKind::AND || expr->GetExprKind() == ExprKind::OR) &&
        expr->GetExprMajorKind() == ExprMajorKind::BINARY_EXPR) {
        if (!ApplyLogicalBoolConstraint(state, StaticCast<const BinaryExpression*>(expr), expr->GetExprKind(), branchCondition)) {
            return false;
        }
        (void)NarrowBoolValue(state, condition, branchCondition);
        return true;
    }
    if (!IsRelationalExprKind(expr->GetExprKind())) {
        return NarrowBoolValue(state, condition, branchCondition);
    }

    auto binary = StaticCast<const BinaryExpression*>(expr);
    auto lhs = binary->GetLHSOperand();
    auto rhs = binary->GetRHSOperand();
    auto rel = ToRelationalOperation(expr->GetExprKind());
    bool feasible = true;
    if (lhs->GetType()->IsInteger() && rhs->GetType()->IsInteger()) {
        feasible = ApplyIntComparisonConstraint(state, lhs, rhs, rel, branchCondition);
    } else if ((expr->GetExprKind() == ExprKind::EQUAL || expr->GetExprKind() == ExprKind::NOTEQUAL) &&
        lhs->GetType()->IsBoolean() && rhs->GetType()->IsBoolean()) {
        feasible = ApplyBoolEqualityConstraint(state, lhs, rhs, rel, branchCondition);
    }
    if (!feasible) {
        return false;
    }
    (void)NarrowBoolValue(state, condition, branchCondition);
    return true;
}

// 对 MultiBranch 的 case/default 后继应用对应约束。
bool ApplyMultiBranchConstraint(RangeDomain& state, const MultiBranch* multi, const Block* successor)
{
    auto cond = multi->GetCondition();
    if (!IsIntegerValue(cond)) {
        return true;
    }
    auto width = ToWidth(*cond->GetType());
    auto isUnsigned = cond->GetType()->IsUnsignedInteger();
    if (successor == multi->GetDefaultBlock()) {
        for (auto caseVal : multi->GetCaseVals()) {
            if (!NarrowSIntValue(
                state, cond, SIntDomain::FromNumeric(RelationalOperation::NE, SInt{width, caseVal}, isUnsigned))) {
                return false;
            }
            auto source = GetSameWidthTypeCastSource(cond);
            if (source != nullptr && source != cond) {
                if (!NarrowMultiBranchTarget(state, source, RelationalOperation::NE, caseVal)) {
                    return false;
                }
            }
        }
        return true;
    }

    std::optional<uint64_t> matchedCase;
    for (size_t i = 0; i < multi->GetCaseVals().size(); ++i) {
        if (successor != multi->GetCaseBlockByIndex(i)) {
            continue;
        }
        if (matchedCase.has_value()) {
            return true;
        }
        matchedCase = multi->GetCaseValByIndex(i);
    }
    if (matchedCase.has_value()) {
        return NarrowMultiBranchCondition(state, cond, RelationalOperation::EQ, matchedCase.value());
    }
    return true;
}

// 带 visited 集合检查 CFG 可达性，用于循环检测。
bool CanReachBlock(const Block* start, const Block* target, std::unordered_set<const Block*>& visited)
{
    if (start == nullptr || target == nullptr || !visited.emplace(start).second) {
        return false;
    }
    if (start == target) {
        return true;
    }
    for (auto successor : start->GetSuccessors()) {
        if (CanReachBlock(successor, target, visited)) {
            return true;
        }
    }
    return false;
}

// 判断分支是否有后继路径回到自身 block。
bool IsLoopBranch(const Branch* branch)
{
    auto parent = branch->GetParentBlock();
    std::unordered_set<const Block*> visited;
    if (CanReachBlock(branch->GetTrueBlock(), parent, visited)) {
        return true;
    }
    visited.clear();
    return CanReachBlock(branch->GetFalseBlock(), parent, visited);
}
} // namespace

// 识别简单归纳变量的 i +/- const 更新，并直接推导更新表达式结果范围。
std::optional<SIntDomain> RangeAnalysis::TryComputeSimpleInductionUpdateRange(
    const BinaryExpression* binaryExpr) const
{
    auto dest = binaryExpr->GetResult();
    if (!dest->GetType()->IsInteger() || dest->GetType()->IsUnsignedInteger()) {
        return std::nullopt;
    }

    auto lhs = binaryExpr->GetLHSOperand();
    auto rhs = binaryExpr->GetRHSOperand();
    Value* loadValue = nullptr;
    Value* location = nullptr;
    std::optional<int64_t> updateStep;
    switch (binaryExpr->GetExprKind()) {
        case ExprKind::ADD:
            if ((location = GetLoadLocation(lhs)) != nullptr) {
                loadValue = lhs;
                updateStep = GetSignedConstantFromDefiningConstant(rhs);
            } else if ((location = GetLoadLocation(rhs)) != nullptr) {
                loadValue = rhs;
                updateStep = GetSignedConstantFromDefiningConstant(lhs);
            }
            break;
        case ExprKind::SUB:
            location = GetLoadLocation(lhs);
            if (location != nullptr) {
                loadValue = lhs;
                auto rhsStep = GetSignedConstantFromDefiningConstant(rhs);
                updateStep = rhsStep.has_value() ? NegateSignedStep(rhsStep.value()) : std::nullopt;
            }
            break;
        default:
            break;
    }
    if (location == nullptr || loadValue == nullptr || !updateStep.has_value() || updateStep.value() == 0) {
        return std::nullopt;
    }

    auto loadExpr = GetDefiningExpr(loadValue);
    if (loadExpr == nullptr || loadExpr->GetExprKind() != ExprKind::LOAD) {
        return std::nullopt;
    }
    auto header = loadExpr->GetParentBlock();
    auto terminator = header == nullptr ? nullptr : header->GetTerminator();
    if (terminator == nullptr || terminator->GetExprKind() != ExprKind::BRANCH) {
        return std::nullopt;
    }
    auto branch = StaticCast<const Branch*>(terminator);
    if (branch->GetSourceExpr() != SourceExpr::WHILE_EXPR && branch->GetSourceExpr() != SourceExpr::DO_WHILE_EXPR) {
        return std::nullopt;
    }

    auto condition = GetSimpleInductionCondition(branch->GetCondition());
    if (!condition.has_value() || condition->location != location) {
        return std::nullopt;
    }
    auto loopStep = FindSingleBackedgeStep(header, location);
    auto init = FindIncomingSignedStoreConstant(header, location);
    if (!loopStep.has_value() || !init.has_value() || loopStep.value() != updateStep.value()) {
        return std::nullopt;
    }

    auto width = ToWidth(*dest->GetType());
    auto exactExit =
        ComputeExactInductionExit(init.value(), loopStep.value(), condition->relation, condition->bound, width);
    if (!exactExit.has_value()) {
        return std::nullopt;
    }
    __int128 firstUpdate = static_cast<__int128>(init.value()) + static_cast<__int128>(updateStep.value());
    if (!FitsSignedWidth(firstUpdate, width)) {
        return std::nullopt;
    }
    __int128 lastUpdate = static_cast<__int128>(exactExit->SVal());
    __int128 lower = firstUpdate < lastUpdate ? firstUpdate : lastUpdate;
    __int128 upper = firstUpdate < lastUpdate ? lastUpdate : firstUpdate;
    if (!FitsSignedWidth(lower, width) || !FitsSignedWidth(upper, width)) {
        return std::nullopt;
    }

    auto lowerValue = SInt{width, static_cast<uint64_t>(static_cast<int64_t>(lower))};
    auto upperValue = SInt{width, static_cast<uint64_t>(static_cast<int64_t>(upper))};
    return SIntDomain::Intersects(SIntDomain::FromNumeric(RelationalOperation::GE, lowerValue, false),
        SIntDomain::FromNumeric(RelationalOperation::LE, upperValue, false));
}

// 处理终结符转移效果，并折叠可证明确定的终结符。
std::optional<Block*> RangeAnalysis::HandleTerminatorEffect(RangeDomain& state, const Terminator* terminator)
{
    RangeAnalysis::ExceptionKind res = ExceptionKind::NA;
    switch (terminator->GetExprKind()) {
        case ExprKind::GOTO:
        case ExprKind::EXIT:
            break;
        case ExprKind::BRANCH:
            return HandleBranchTerminator(state, StaticCast<const Branch*>(terminator));
        case ExprKind::MULTIBRANCH:
            return HandleMultiBranchTerminator(state, StaticCast<const MultiBranch*>(terminator));
        case ExprKind::TYPECAST_WITH_EXCEPTION:
            res = HandleTypeCast(state, StaticCast<const TypeCastWithException*>(terminator));
            break;
        case ExprKind::INT_OP_WITH_EXCEPTION:
        case ExprKind::INTRINSIC_WITH_EXCEPTION:
        default: {
            auto dest = terminator->GetResult();
            if (dest) {
                state.SetToTopOrTopRef(dest, dest->GetType()->IsRef());
            }
            break;
        }
    }
    if (res == ExceptionKind::SUCCESS) {
        return terminator->GetSuccessor(0);
    } else if (res == ExceptionKind::FAIL) {
        return terminator->GetSuccessor(1);
    }

    return std::nullopt;
}

// 为分支和 MultiBranch 后继构造边专属 RangeDomain 状态。
RangeDomain GetTerminatorStateForSuccessor(
    const Analysis<RangeDomain>& analysis, const RangeDomain& state, const Terminator* terminator, const Block* successor)
{
    (void)analysis;
    auto edgeState = state;
    switch (terminator->GetExprKind()) {
        case ExprKind::BRANCH: {
            auto branch = StaticCast<const Branch*>(terminator);
            if (branch->GetTrueBlock() == branch->GetFalseBlock()) {
                return edgeState;
            }
            if (successor == branch->GetTrueBlock()) {
                if (!ApplyConditionConstraint(edgeState, branch->GetCondition(), true)) {
                    edgeState.SetUnreachable();
                }
            } else if (successor == branch->GetFalseBlock()) {
                if (!ApplyConditionConstraint(edgeState, branch->GetCondition(), false)) {
                    edgeState.SetUnreachable();
                } else if (!TryNarrowSimpleInductionExit(edgeState, branch, successor)) {
                    edgeState.SetUnreachable();
                }
            }
            break;
        }
        case ExprKind::MULTIBRANCH:
            if (!ApplyMultiBranchConstraint(edgeState, StaticCast<const MultiBranch*>(terminator), successor)) {
                edgeState.SetUnreachable();
            }
            break;
        default:
            break;
    }
    return edgeState;
}

void RangeAnalysis::PrintBranchOptMessage(const Ptr<const Expression>& expr, bool isTrueBlockRemained) const
{
    std::string message = "[RangeAnalysis] The If Block" + ToPosInfo(expr->GetDebugLocation()) +
        " has been replace to the " + (isTrueBlockRemained ? "True Block" : "False Block") + "\n";
    std::cout << message;
}

// 仅在条件为单点且不是循环 guard 时选择分支后继。
std::optional<Block*> RangeAnalysis::HandleBranchTerminator(const RangeDomain& state, const Branch* branch) const
{
    if (IsLoopBranch(branch)) {
        return std::nullopt;
    }
    auto cond = branch->GetCondition();
    const auto& condVal = GetBoolDomainFromState(state, cond);
    if (!condVal.IsSingleValue()) {
        return std::nullopt;
    }
    if (isDebug) {
        PrintBranchOptMessage(branch, condVal.IsTrue());
    }
    return condVal.IsTrue() ? branch->GetTrueBlock() : branch->GetFalseBlock();
}

// 在条件为单点 case 值时选择 MultiBranch 后继。
std::optional<Block*> RangeAnalysis::HandleMultiBranchTerminator(
    const RangeDomain& state, const MultiBranch* multi) const
{
    auto cond = multi->GetCondition();
    const auto& condVal = GetSIntDomainFromState(state, cond);
    if (!condVal.IsSingleValue()) {
        return std::nullopt;
    }
    auto val = condVal.NumericBound().Lower().UVal();
    auto cases = multi->GetCaseVals();
    for (size_t i = 0; i < cases.size(); ++i) {
        if (val == cases[i]) {
            return multi->GetSuccessor(i + 1);
        }
    }
    return multi->GetDefaultBlock();
}
} // namespace Cangjie::CHIR
