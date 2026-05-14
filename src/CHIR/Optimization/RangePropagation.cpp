// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/Optimization/RangePropagation.h"
#include "cangjie/CHIR/Analysis/ConstAnalysis.h"
#include "cangjie/CHIR/Analysis/Engine.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <optional>

namespace Cangjie::CHIR {

namespace {
const std::string CONTEST_INPUT_FILE = "input.txt";
const std::string CONTEST_OUTPUT_FILE = "output.txt";

struct ContestQuery {
    std::string fileName;
    unsigned line{0};
    std::string variableName;
    std::string result;
    Type* type{nullptr};
    bool valid{true};
    bool resolved{false};
};

std::string Trim(const std::string& str)
{
    auto begin = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

std::string BaseName(const std::string& path)
{
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::vector<std::string> SplitCommaSeparated(const std::string& str)
{
    std::vector<std::string> parts;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        parts.emplace_back(Trim(item));
    }
    return parts;
}

ContestQuery MakeInvalidContestQuery()
{
    ContestQuery query;
    query.valid = false;
    return query;
}

ContestQuery ParseContestQueryLine(const std::string& line)
{
    auto trimmed = Trim(line);
    if (trimmed.empty()) {
        return MakeInvalidContestQuery();
    }
    if (trimmed.front() != '[' || trimmed.back() != ']') {
        return MakeInvalidContestQuery();
    }
    auto parts = SplitCommaSeparated(trimmed.substr(1, trimmed.size() - 2));
    if (parts.size() != 3) {
        return MakeInvalidContestQuery();
    }
    ContestQuery query;
    query.fileName = BaseName(parts[0]);
    try {
        size_t parsedSize = 0;
        auto parsedLine = std::stoul(parts[1], &parsedSize);
        if (parsedSize != parts[1].size() || parsedLine > std::numeric_limits<unsigned>::max()) {
            return MakeInvalidContestQuery();
        }
        query.line = static_cast<unsigned>(parsedLine);
    } catch (...) {
        return MakeInvalidContestQuery();
    }
    query.variableName = parts[2];
    return query;
}

std::optional<std::vector<ContestQuery>> LoadContestQueries()
{
    std::ifstream input(CONTEST_INPUT_FILE);
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::vector<ContestQuery> queries;
    std::string line;
    while (std::getline(input, line)) {
        queries.emplace_back(ParseContestQueryLine(line));
    }
    return queries;
}

Type* GetQueryValueType(Value* value)
{
    auto type = value->GetType();
    if (type->IsRef()) {
        return StaticCast<RefType*>(type)->GetRootBaseType();
    }
    return type;
}

std::string FormatSignedInt(int64_t value)
{
    std::stringstream ss;
    ss << value;
    return ss.str();
}

std::string FormatUnsignedInt(uint64_t value)
{
    std::stringstream ss;
    ss << value;
    return ss.str();
}

std::string FormatSIntValue(const SInt& value, const Type& type)
{
    return type.IsUnsignedInteger() ? FormatUnsignedInt(value.UVal()) : FormatSignedInt(value.SVal());
}

bool IsContestPrintableIntegerInterval(const ConstantRange& range, const Type& type)
{
    if (range.IsFullSet() || range.IsEmptySet()) {
        return false;
    }
    return type.IsUnsignedInteger() ? !range.IsWrappedSet() : !range.IsSignWrappedSet();
}

std::string FormatContestIntegerInterval(const ConstantRange& range, const Type& type)
{
    auto upperInclusive = range.Upper() - 1U;
    std::stringstream ss;
    ss << "[" << FormatSIntValue(range.Lower(), type) << ", " << FormatSIntValue(upperInclusive, type) << ":1]";
    return ss.str();
}

std::string FormatFullIntegerRange(const Type& type)
{
    auto width = ToWidth(type);
    std::stringstream ss;
    if (type.IsUnsignedInteger()) {
        ss << "[" << SInt::UMinValue(width).UVal() << ", " << SInt::UMaxValue(width).UVal() << ":1]";
    } else {
        ss << "[" << SInt::SMinValue(width).SVal() << ", " << SInt::SMaxValue(width).SVal() << ":1]";
    }
    return ss.str();
}

std::string FormatFallback(Type* type)
{
    if (type && type->IsBoolean()) {
        return "false, true";
    }
    if (type && type->IsInteger()) {
        return FormatFullIntegerRange(*type);
    }
    return "[-9223372036854775808, 9223372036854775807:1]";
}

std::string FormatContestRange(const ValueRange* range, Type* type)
{
    if (!type || !range) {
        return FormatFallback(type);
    }
    if (range->GetRangeKind() == ValueRange::RangeKind::BOOL) {
        const auto& boolRange = StaticCast<const BoolRange&>(*range).GetVal();
        if (boolRange.IsTrue()) {
            return "true";
        }
        if (boolRange.IsFalse()) {
            return "false";
        }
        return "false, true";
    }
    if (range->GetRangeKind() == ValueRange::RangeKind::SINT) {
        const auto& intRange = StaticCast<const SIntRange&>(*range).GetVal();
        const auto& numeric = intRange.NumericBound();
        if (intRange.IsSingleValue()) {
            return FormatSIntValue(numeric.GetSingleElement(), *type);
        }
        if (IsContestPrintableIntegerInterval(numeric, *type)) {
            return FormatContestIntegerInterval(numeric, *type);
        }
        return FormatFallback(type);
    }
    return FormatFallback(type);
}

bool IsSameQueryLocation(const ContestQuery& query, const DebugLocation& location)
{
    return query.fileName == BaseName(location.GetFileName()) && query.line == location.GetBeginPos().line;
}

void RememberQueryType(std::vector<ContestQuery>& queries, const Debug& debug)
{
    auto type = GetQueryValueType(debug.GetValue());
    for (auto& query : queries) {
        if (!query.valid || query.type || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (query.fileName == BaseName(debug.GetDebugLocation().GetFileName()) || query.fileName.empty()) {
            query.type = type;
        }
    }
}

void ResolveQueryAtDebug(std::vector<ContestQuery>& queries, const Debug& debug, const RangeDomain& state)
{
    RememberQueryType(queries, debug);
    auto type = GetQueryValueType(debug.GetValue());
    auto range = state.CheckAbstractValue(debug.GetValue());
    for (auto& query : queries) {
        if (!query.valid || query.resolved || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (!IsSameQueryLocation(query, debug.GetDebugLocation())) {
            continue;
        }
        query.type = type;
        query.result = FormatContestRange(range, type);
        query.resolved = true;
    }
}

void WriteContestOutput(std::vector<ContestQuery>& queries)
{
    std::ofstream output(CONTEST_OUTPUT_FILE, std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    for (auto& query : queries) {
        output << (query.resolved ? query.result : FormatFallback(query.type)) << '\n';
    }
}
} // namespace

std::optional<bool> CheckSingleBool(const ValueRange& vr)
{
    if (vr.GetRangeKind() == ValueRange::RangeKind::BOOL) {
        auto& boolRange = StaticCast<BoolRange>(vr);
        if (boolRange.GetVal().IsSingleValue()) {
            return boolRange.GetVal().GetSingleValue();
        }
    }
    return std::nullopt;
}

std::optional<SInt> CheckSingleSInt(const ValueRange& vr)
{
    if (vr.GetRangeKind() == ValueRange::RangeKind::SINT) {
        auto& sIntRange = StaticCast<SIntRange>(vr);
        if (sIntRange.GetVal().IsSingleValue()) {
            return sIntRange.GetVal().NumericBound().GetSingleElement();
        }
    }
    return std::nullopt;
}

RangePropagation::RangePropagation(
    CHIRBuilder& builder, RangeAnalysisWrapper* rangeAnalysisWrapper, DiagAdapter* diag, bool enIncre)
    : builder(builder), analysisWrapper(rangeAnalysisWrapper), diag(diag), enIncre(enIncre)
{
}

const OptEffectCHIRMap& RangePropagation::GetEffectMap() const
{
    return effectMap;
}

const std::vector<const Func*>& RangePropagation::GetFuncsNeedRemoveBlocks() const
{
    return funcsNeedRemoveBlocks;
}

void RangePropagation::RunOnPackage(const Ptr<const Package>& package, bool isDebug)
{
    for (auto func : package->GetGlobalFuncs()) {
        RunOnFunc(func, isDebug);
    }
}

void RangePropagation::RunOnFunc(const Ptr<const Func>& func, bool isDebug)
{
    auto result = analysisWrapper->CheckFuncResult(func);
    if (!result) {
        return;
    }
    std::vector<RewriteInfo> toBeRewrited;
    const auto actionBeforeVisitExpr = [](const RangeDomain&, Expression*, size_t) {};
    const auto actionAfterVisitExpr = [this, &toBeRewrited, func](
                                          const RangeDomain& state, Expression* expr, size_t index) {
        auto exprType = expr->GetResult()->GetType();
        if (expr->IsBinaryExpr()) {
            if (auto absVal = state.CheckAbstractValue(expr->GetResult()); absVal) {
                return (void)toBeRewrited.emplace_back(expr, index, GenerateConstExpr(exprType, absVal));
            }
        } else if (expr->IsUnaryExpr()) {
            if (auto absVal = state.CheckAbstractValue(expr->GetResult()); absVal) {
                return (void)toBeRewrited.emplace_back(expr, index, GenerateConstExpr(exprType, absVal));
            }
        } else if ((exprType->IsInteger() || exprType->IsBoolean()) &&
            (expr->IsLoad() || expr->IsTypeCast() || expr->IsField())) {
            if (auto absVal = state.CheckAbstractValue(expr->GetResult()); absVal && !exprType->IsString()) {
                toBeRewrited.emplace_back(expr, index, GenerateConstExpr(exprType, absVal));
                RecordEffectMap(expr, func);
            }
        } else if (expr->GetExprKind() == ExprKind::INTRINSIC) {
            if (auto intrinic = StaticCast<Intrinsic*>(expr);
                intrinic->GetIntrinsicKind() == CHIR::IntrinsicKind::VARRAY_SET ||
                intrinic->GetIntrinsicKind() == CHIR::IntrinsicKind::VARRAY_GET) {
                CheckVarrayIndex(intrinic, state);
            }
        }
    };
    bool doBlockElimination = false;
    const auto actionOnTerminator = [this, isDebug, &doBlockElimination](const RangeDomain&, Terminator* terminator,
                                        std::optional<Block*> targetSucc) {
        switch (terminator->GetExprKind()) {
            case ExprKind::BRANCH:
            case ExprKind::MULTIBRANCH:
                if (targetSucc.has_value()) {
                    doBlockElimination = true;
                    return RewriteBranchTerminator(terminator, targetSucc.value(), isDebug);
                }
                break;
            default:
                break;
        }
    };
    result->VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
    for (auto& rewriteInfo : toBeRewrited) {
        RewriteToConstExpr(rewriteInfo, isDebug);
    }
    if (doBlockElimination) {
        funcsNeedRemoveBlocks.push_back(func.get());
    }
}

void RangePropagation::EmitContestOutput(const Ptr<const Package>& package, RangeAnalysisWrapper& rangeAnalysisWrapper)
{
    auto queries = LoadContestQueries();
    if (!queries.has_value()) {
        return;
    }
    const auto actionBeforeVisitExpr = [](const RangeDomain&, Expression*, size_t) {};
    auto actionAfterVisitExpr = [&queries](const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() != ExprKind::DEBUGEXPR) {
            return;
        }
        ResolveQueryAtDebug(queries.value(), *StaticCast<Debug*>(expr), state);
    };
    const auto actionOnTerminator = [](const RangeDomain&, Terminator*, std::optional<Block*>) {};
    for (auto func : package->GetGlobalFuncs()) {
        auto result = rangeAnalysisWrapper.CheckFuncResult(func);
        if (!result) {
            continue;
        }
        result->VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
    }
    WriteContestOutput(queries.value());
}

Ptr<LiteralValue> RangePropagation::GenerateConstExpr(const Ptr<Type>& type, const Ptr<const ValueRange>& rangeVal)
{
    switch (rangeVal->GetRangeKind()) {
        case ValueRange::RangeKind::BOOL:
            if (auto boolValue = CheckSingleBool(*rangeVal.get())) {
                return builder.CreateLiteralValue<BoolLiteral>(type, boolValue.value());
            }
            break;
        case ValueRange::RangeKind::SINT:
            if (auto intValue = CheckSingleSInt(*rangeVal.get())) {
                return builder.CreateLiteralValue<IntLiteral>(type, intValue.value().UVal());
            }
            break;
    }
    return nullptr;
}

void RangePropagation::RewriteToConstExpr(const RewriteInfo& rewriteInfo, bool isDebug) const
{
    if (!rewriteInfo.literalVal) {
        return;
    }
    auto oldExpr = rewriteInfo.oldExpr;
    auto oldExprResult = oldExpr->GetResult();
    auto oldExprParent = oldExpr->GetParentBlock();
    auto newExpr = builder.CreateExpression<Constant>(oldExprResult->GetType(), rewriteInfo.literalVal, oldExprParent);
    newExpr->SetDebugLocation(oldExpr->GetDebugLocation());

    oldExprParent->GetExpressionByIdx(rewriteInfo.index)->ReplaceWith(*newExpr);

    if (isDebug) {
        std::string message = "[RangePropagation] The " +
            ExprKindMgr::Instance()->GetKindName(static_cast<size_t>(oldExpr->GetExprKind())) +
            ToPosInfo(oldExpr->GetDebugLocation()) + " has been rewrited to a constant\n";
        std::cout << message;
    }
}

void RangePropagation::RewriteBranchTerminator(
    const Ptr<Terminator>& branch, const Ptr<Block>& targetSucc, bool isDebug)
{
    auto parentBlock = branch->GetParentBlock();
    branch->RemoveSelfFromBlock();
    auto newTerminator = builder.CreateTerminator<GoTo>(targetSucc, parentBlock);
    parentBlock->AppendExpression(newTerminator);
    if (isDebug) {
        std::string message = "[RangePropagation] The terminator " +
            ExprKindMgr::Instance()->GetKindName(static_cast<size_t>(branch->GetExprKind())) +
            ToPosInfo(branch->GetDebugLocation()) + " has been optimised\n";
        std::cout << message;
    }
}

GlobalVar* RecordLoadEffectMap(const Ptr<const Load>& load)
{
    GlobalVar* gv = nullptr;
    auto loc = load->GetLocation();
    if (loc->IsGlobalVarInCurPackage()) {
        // let a = 3
        // Load(gv_a)
        gv = DynamicCast<GlobalVar*>(loc);
    } else if (loc->IsLocalVar()) {
        // let sa = SA(); sa.x
        // %0 = GetElementRef(gv_sa); %1 = Load(%0)
        auto locExpr = StaticCast<LocalVar*>(loc)->GetExpr();
        if (locExpr->GetExprKind() == ExprKind::GET_ELEMENT_REF) {
            auto base = StaticCast<GetElementRef*>(locExpr)->GetLocation();
            if (base->IsGlobalVarInCurPackage()) {
                gv = DynamicCast<GlobalVar*>(base);
            }
        }
    }
    return gv;
}

GlobalVar* RecordFieldEffectMap(const Ptr<const Field>& field)
{
    GlobalVar* gv = nullptr;
    auto base = field->GetBase();
    if (base->IsLocalVar()) {
        auto baseExpr = StaticCast<LocalVar*>(base)->GetExpr();
        if (baseExpr->GetExprKind() == ExprKind::LOAD) {
            auto loc = StaticCast<Load*>(baseExpr)->GetLocation();
            if (loc->IsGlobalVarInCurPackage()) {
                // let a = (1, 2); a[0]
                // %0 = Load(gv_a); %1 = Field(%0, 0)
                gv = DynamicCast<GlobalVar*>(loc);
            }
        }
    }
    return gv;
}

static std::mutex g_mtx;
OptEffectCHIRMap RangePropagation::effectMap;
void RangePropagation::RecordEffectMap(const Expression* expr, const Func* func) const
{
    if (!enIncre) {
        return;
    }
    GlobalVar* gv = nullptr;
    if (expr->GetExprKind() == ExprKind::LOAD) {
        gv = RecordLoadEffectMap(StaticCast<Load*>(expr));
    } else if (expr->GetExprKind() == ExprKind::FIELD) {
        gv = RecordFieldEffectMap(StaticCast<Field*>(expr));
    }
    if (gv) {
        std::lock_guard<std::mutex> guard(g_mtx);
        effectMap[gv].emplace(const_cast<Func*>(func));
    }
}

std::vector<size_t> GetVArraySizeList(const Ptr<Type>& type)
{
    std::vector<size_t> size;
    auto indexType = type.get();
    if (indexType->IsRef()) {
        indexType = StaticCast<RefType>(indexType)->GetBaseType();
    }
    while (indexType->GetTypeKind() == Type::TypeKind::TYPE_VARRAY) {
        auto vArrayType = StaticCast<const VArrayType*>(indexType);
        size.push_back(vArrayType->GetSize());
        indexType = vArrayType->GetElementType();
    }
    return size;
}

void RangePropagation::CheckVarrayIndex(const Ptr<Intrinsic>& intrin, const RangeDomain& state) const
{
    CJC_ASSERT(intrin->GetIntrinsicKind() == CHIR::IntrinsicKind::VARRAY_GET ||
        intrin->GetIntrinsicKind() == CHIR::IntrinsicKind::VARRAY_SET);
    auto& args = intrin->GetArgs();
    CJC_ASSERT(args.size() >= 2U);
    size_t begin = intrin->GetIntrinsicKind() == CHIR::IntrinsicKind::VARRAY_GET ? 1U : 2U;
    auto sizes = GetVArraySizeList(args[0]->GetType());
    CJC_ASSERT(sizes.size() >= args.size() - begin);
    for (size_t i = begin; i < args.size(); ++i) {
        auto size = sizes[i - begin];
        auto index = args[i];
        auto indexRange = RangeAnalysis::GetSIntDomainFromState(state, index);
        if (indexRange.IsTop()) {
            return;
        }
        SIntDomain varraySizeNode{ConstantRange{SInt{IntWidth::I64, static_cast<uint64_t>(size)}}, false};
        SIntDomain zeroNode{ConstantRange{SInt::Zero(IntWidth::I64)}, false};
        auto ltUpperBound{ComputeRelIntBinop({indexRange, varraySizeNode, index, nullptr, ExprKind::LT, false})};
        auto geLowerBound{ComputeRelIntBinop({indexRange, zeroNode, index, nullptr, ExprKind::GE, false})};
        if (ltUpperBound.IsFalse() || geLowerBound.IsFalse()) {
            auto bd =
                diag->DiagnoseRefactor(DiagKindRefactor::chir_idx_out_of_bounds, ToRange(intrin->GetDebugLocation()));
            std::stringstream ss;
            ss << "range of index " << i - begin << " is (" << indexRange.ToString()
               << "), however the size of varray is " + std::to_string(size);
            bd.AddMainHintArguments(ss.str());
        }
    }
}
} // namespace Cangjie::CHIR
