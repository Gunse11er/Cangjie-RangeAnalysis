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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Cangjie::CHIR {

namespace {
const std::string CONTEST_INPUT_FILE = "input.txt";
const std::string CONTEST_OUTPUT_FILE = "output.txt";

struct ContestInputContext {
    std::filesystem::path inputPath;
    std::filesystem::path rootPath;
};

std::optional<std::filesystem::path> FindContestInputFileFrom(const std::filesystem::path& start)
{
    std::error_code ec;
    if (start.empty()) {
        return std::nullopt;
    }
    auto current = start;
    if (current.is_relative()) {
        auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            return std::nullopt;
        }
        current = cwd / current;
    }
    std::vector<std::filesystem::path> starts{current};
    auto parent = current.parent_path();
    if (!parent.empty() && parent != current) {
        starts.emplace_back(parent);
    }
    for (auto probe : starts) {
        ec.clear();
        while (true) {
            auto candidate = probe / CONTEST_INPUT_FILE;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate.lexically_normal();
            }
            ec.clear();
            if (probe == probe.root_path()) {
                break;
            }
            auto next = probe.parent_path();
            if (next.empty() || next == probe) {
                break;
            }
            probe = next;
        }
    }
    return std::nullopt;
}

std::optional<ContestInputContext> FindContestInputContext(const std::vector<std::string>& contestRootHints)
{
    for (const auto& hint : contestRootHints) {
        auto inputPath = FindContestInputFileFrom(std::filesystem::path(hint));
        if (!inputPath.has_value()) {
            continue;
        }
        return ContestInputContext{inputPath.value(), inputPath.value().parent_path()};
    }
    return std::nullopt;
}

bool IsGlobalVarInCurrentPackage(const Value* value)
{
    return value != nullptr && value->IsGlobalVar() && !value->IsImportedVar();
}

enum class ContestQueryTypeHint {
    UNKNOWN,
    BOOL,
    INT8,
    INT16,
    INT32,
    INT64,
    UINT8,
    UINT16,
    UINT32,
    UINT64,
};

enum class ContestResultOrigin {
    NONE,
    CHIR_ANALYSIS,
    CONTEXT_SUMMARY,
    UNRESOLVED_CHIR_TOP,
};
struct ContestQuery {
    std::string fileName;
    std::string sourceFileName;
    std::string fileKey;
    unsigned line{0};
    std::string variableName;
    std::string result;
    std::shared_ptr<ValueRange> resultRange;
    Type* type{nullptr};
    GlobalVar* boundGlobal{nullptr};
    bool isGlobalDeclarationQuery{false};
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    ContestResultOrigin resultOrigin{ContestResultOrigin::NONE};
    bool valid{true};
    bool resolved{false};
    bool hasBeforePointResult{false};
    bool hasDirectPointLoadResult{false};
    bool hasUnknownPointObservation{false};
    bool hasBoundedLifetimeResult{false};
    bool hasBoundedPointResult{false};
    bool hasDeclarationStoreResult{false};
    bool hasReadModifyWriteResult{false};
};

struct ValueNameInfo {
    std::string name;
    std::string fileKey;
    unsigned line{0};
    std::vector<int> scopeInfo;
};

using ValueNameMap = std::unordered_map<Value*, std::vector<ValueNameInfo>>;

struct ContestAggregateBinding {
    Value* value{nullptr};
    const Store* initializationStore{nullptr};
};

struct ContestAggregate {
    Type* type{nullptr};
    std::vector<ContestAggregateBinding> bindings;
    bool bindingsOverflow{false};
};

using ContestAggregateMap = std::unordered_map<std::string, ContestAggregate>;

struct ContestContextCandidate {
    Type* type{nullptr};
    std::unique_ptr<ValueRange> range;
    bool fromGlobalAccess{false};
    bool requiresInitializerObservation{false};
    bool sawInitializerObservation{false};
    bool sawExitObservation{false};
    bool incompleteGlobalLifetime{false};
    bool hasUnknownObservation{false};
    bool auxiliaryOnly{false};
};

using ContestContextCandidateMap = std::unordered_map<size_t, ContestContextCandidate>;
constexpr size_t MAX_CONTEST_AGGREGATE_BINDINGS = 64;
constexpr size_t MAX_CONTEST_LOOP_USES = 256;

enum class ContestReachability : uint8_t {
    UNREACHABLE,
    REACHABLE,
    UNKNOWN
};

ContestReachability CanReachBlockForQueryMapping(const Block* start, const Block* target);
Expression* GetLocalDefiningExpression(Value* value);

// 去除竞赛输入字段首尾空白。
std::string Trim(const std::string& str)
{
    auto begin = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

// 提取文件名 basename，用于匹配 DebugLocation。
std::string BaseName(const std::string& path)
{
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool HasDirectoryPart(const std::string& path)
{
    return path.find('/') != std::string::npos || path.find('\\') != std::string::npos;
}

std::string NormalizePathKey(const std::filesystem::path& path)
{
    auto key = path.lexically_normal().generic_string();
    if (key.rfind("./", 0) == 0) {
        key = key.substr(2);
    }
    return key;
}

std::string NormalizeQueryFileKey(const std::string& fileName, const std::filesystem::path& contestRoot)
{
    std::filesystem::path path(fileName);
    if (path.is_absolute()) {
        auto rel = path.lexically_relative(contestRoot);
        if (!rel.empty() && rel.native().find("..") != 0) {
            return NormalizePathKey(rel);
        }
    }
    return NormalizePathKey(path);
}

std::string GetLocationFileKey(const DebugLocation& location, const std::filesystem::path& contestRoot)
{
    std::filesystem::path path(location.GetAbsPath());
    if (path.is_absolute()) {
        auto rel = path.lexically_relative(contestRoot);
        if (!rel.empty() && rel.native().find("..") != 0) {
            return NormalizePathKey(rel);
        }
    }
    return NormalizePathKey(path);
}

std::string MakeContestAggregateKey(const std::string& fileKey, unsigned line, const std::string& variableName)
{
    return fileKey + "\0" + std::to_string(line) + "\0" + variableName;
}

// 按逗号切分查询字段并清理每个字段。
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

// 解析一行 [file, line, variable] 格式的竞赛查询。
ContestQuery ParseContestQueryLine(const std::string& line, const std::filesystem::path& contestRoot)
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
    query.sourceFileName = parts[0];
    query.fileName = BaseName(query.sourceFileName);
    query.fileKey = NormalizeQueryFileKey(query.sourceFileName, contestRoot);
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

// 当 input.txt 存在时读取全部竞赛查询。
std::optional<std::vector<ContestQuery>> LoadContestQueries(const ContestInputContext& inputContext)
{
    std::ifstream input(inputContext.inputPath.string());
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::vector<ContestQuery> queries;
    std::string line;
    while (std::getline(input, line)) {
        queries.emplace_back(ParseContestQueryLine(line, inputContext.rootPath));
    }
    return queries;
}

// 获取查询输出时使用的源类型，ref 查询取其根类型。
Type* GetQueryValueType(Value* value)
{
    auto type = value->GetType();
    if (type->IsRef()) {
        return StaticCast<RefType*>(type)->GetRootBaseType();
    }
    return type;
}

// 将 CHIR Type* 转为轻量类型提示，供未解析查询生成正确 fallback。
ContestQueryTypeHint GetQueryTypeHint(Type* type)
{
    if (type == nullptr) {
        return ContestQueryTypeHint::UNKNOWN;
    }
    if (type->IsRef()) {
        type = StaticCast<RefType*>(type)->GetRootBaseType();
    }
    if (type->IsBoolean()) {
        return ContestQueryTypeHint::BOOL;
    }
    if (!type->IsInteger()) {
        return ContestQueryTypeHint::UNKNOWN;
    }
    auto isUnsigned = type->IsUnsignedInteger();
    switch (ToWidth(*type)) {
        case IntWidth::I8:
            return isUnsigned ? ContestQueryTypeHint::UINT8 : ContestQueryTypeHint::INT8;
        case IntWidth::I16:
            return isUnsigned ? ContestQueryTypeHint::UINT16 : ContestQueryTypeHint::INT16;
        case IntWidth::I32:
            return isUnsigned ? ContestQueryTypeHint::UINT32 : ContestQueryTypeHint::INT32;
        case IntWidth::I64:
            return isUnsigned ? ContestQueryTypeHint::UINT64 : ContestQueryTypeHint::INT64;
        default:
            return ContestQueryTypeHint::UNKNOWN;
    }
}

// 将有符号整数端点格式化为竞赛输出文本。
std::string FormatSignedInt(int64_t value)
{
    std::stringstream ss;
    ss << value;
    return ss.str();
}

// 将无符号整数端点格式化为竞赛输出文本。
std::string FormatUnsignedInt(uint64_t value)
{
    std::stringstream ss;
    ss << value;
    return ss.str();
}

// 根据查询类型的符号属性格式化 SInt 端点。
std::string FormatSIntValue(const SInt& value, const Type& type)
{
    return type.IsUnsignedInteger() ? FormatUnsignedInt(value.UVal()) : FormatSignedInt(value.SVal());
}

// 判断整数区间是否能以非 wrapped 竞赛区间格式打印。
bool IsContestPrintableIntegerInterval(const ConstantRange& range, const Type& type)
{
    if (range.IsFullSet() || range.IsEmptySet()) {
        return false;
    }
    return type.IsUnsignedInteger() ? !range.IsWrappedSet() : !range.IsSignWrappedSet();
}

// 将可打印整数区间格式化为 [lower, upper:1]。
std::string FormatContestIntegerInterval(const ConstantRange& range, const Type& type)
{
    auto upperInclusive = range.Upper() - 1U;
    std::stringstream ss;
    ss << "[" << FormatSIntValue(range.Lower(), type) << ", " << FormatSIntValue(upperInclusive, type) << ":1]";
    return ss.str();
}

std::string FormatExactSIntValues(std::vector<SInt> values, const Type& type)
{
    std::sort(values.begin(), values.end(), [&type](const SInt& lhs, const SInt& rhs) {
        return type.IsUnsignedInteger() ? lhs.Ult(rhs) : lhs.Slt(rhs);
    });
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::stringstream ss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << FormatSIntValue(values[i], type);
    }
    return ss.str();
}

// 按整数类型格式化完整 fallback 区间。
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

// 按源码类型提示格式化未解析查询的 sound fallback。
std::string FormatTopRange(ContestQueryTypeHint typeHint)
{
    switch (typeHint) {
        case ContestQueryTypeHint::BOOL:
            return "false, true";
        case ContestQueryTypeHint::INT8:
            return "[-128, 127:1]";
        case ContestQueryTypeHint::INT16:
            return "[-32768, 32767:1]";
        case ContestQueryTypeHint::INT32:
            return "[-2147483648, 2147483647:1]";
        case ContestQueryTypeHint::INT64:
            return "[-9223372036854775808, 9223372036854775807:1]";
        case ContestQueryTypeHint::UINT8:
            return "[0, 255:1]";
        case ContestQueryTypeHint::UINT16:
            return "[0, 65535:1]";
        case ContestQueryTypeHint::UINT32:
            return "[0, 4294967295:1]";
        case ContestQueryTypeHint::UINT64:
            return "[0, 18446744073709551615:1]";
        case ContestQueryTypeHint::UNKNOWN:
            break;
    }
    return "[-9223372036854775808, 9223372036854775807:1]";
}

// 为未解析或不精确查询生成 sound fallback 输出。
std::string FormatTopRange(Type* type)
{
    if (type && type->IsBoolean()) {
        return "false, true";
    }
    if (type && type->IsInteger()) {
        return FormatFullIntegerRange(*type);
    }
    return "[-9223372036854775808, 9223372036854775807:1]";
}

// 优先使用 CHIR Type*，缺失时使用源码类型提示生成 fallback。
std::string FormatTopRange(const ContestQuery& query)
{
    if (query.type != nullptr) {
        return FormatTopRange(query.type);
    }
    return FormatTopRange(query.typeHint);
}

// 将抽象值域转换为竞赛要求的输出格式。
std::string FormatContestRange(const ValueRange* range, Type* type)
{
    if (!type || !range) {
        return FormatTopRange(type);
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
        const auto& sintRange = StaticCast<const SIntRange&>(*range);
        if (sintRange.GetExactValues().has_value()) {
            return FormatExactSIntValues(*sintRange.GetExactValues(), *type);
        }
        const auto& intRange = sintRange.GetVal();
        const auto& numeric = intRange.NumericBound();
        if (intRange.IsSingleValue()) {
            return FormatSIntValue(numeric.GetSingleElement(), *type);
        }
        if (IsContestPrintableIntegerInterval(numeric, *type)) {
            return FormatContestIntegerInterval(numeric, *type);
        }
        return FormatTopRange(type);
    }
    return FormatTopRange(type);
}

// 读取普通 SSA 值或 ref 背后 var 对象的竞赛可见值域。
// Keep Top as a typed lattice value; strings are only an output representation.
std::unique_ptr<ValueRange> MakeContestTopRange(Type* type)
{
    if (type == nullptr) {
        return nullptr;
    }
    if (type->IsRef()) {
        type = StaticCast<RefType*>(type)->GetRootBaseType();
    }
    if (type->IsBoolean()) {
        return std::make_unique<BoolRange>(BoolDomain::Top());
    }
    if (type->IsInteger()) {
        return std::make_unique<SIntRange>(
            SIntDomain::Top(ToWidth(*type), type->IsUnsignedInteger()));
    }
    return nullptr;
}

std::unique_ptr<ValueRange> CloneContestRangeOrTop(Type* type, const ValueRange* range)
{
    return range == nullptr ? MakeContestTopRange(type) : range->Clone();
}

bool AreContestTypesCompatible(Type* lhs, Type* rhs)
{
    return lhs != nullptr && rhs != nullptr && GetQueryTypeHint(lhs) != ContestQueryTypeHint::UNKNOWN &&
        GetQueryTypeHint(lhs) == GetQueryTypeHint(rhs);
}

bool AreContestRangesEquivalent(const ValueRange& lhs, const ValueRange& rhs)
{
    if (lhs.GetRangeKind() != rhs.GetRangeKind()) {
        return false;
    }
    if (lhs.GetRangeKind() == ValueRange::RangeKind::BOOL) {
        return StaticCast<const BoolRange&>(lhs).GetVal().IsSame(
            StaticCast<const BoolRange&>(rhs).GetVal());
    }
    const auto& lhsInt = StaticCast<const SIntRange&>(lhs);
    const auto& rhsInt = StaticCast<const SIntRange&>(rhs);
    return lhsInt.GetVal().IsSame(rhsInt.GetVal()) &&
        lhsInt.GetExactValues() == rhsInt.GetExactValues();
}

bool IsContestRangeSubset(const ValueRange& candidate, const ValueRange& superset)
{
    if (candidate.GetRangeKind() != superset.GetRangeKind()) {
        return false;
    }
    auto joined = superset.Clone();
    if (auto updated = joined->Join(candidate); updated.has_value()) {
        joined = std::move(updated.value());
    }
    return AreContestRangesEquivalent(*joined, superset);
}

bool IsContestTopRange(const ValueRange* range, Type* type)
{
    if (range == nullptr) {
        return true;
    }
    auto top = MakeContestTopRange(type);
    return top != nullptr && top->GetRangeKind() == range->GetRangeKind() &&
        AreContestRangesEquivalent(*range, *top);
}

void SetContestQueryResult(
    ContestQuery& query, Type* type, const ValueRange* range, ContestResultOrigin origin)
{
    if (type != nullptr) {
        query.type = type;
        query.typeHint = GetQueryTypeHint(type);
    }
    auto semanticRange = CloneContestRangeOrTop(type, range);
    query.resultRange = std::move(semanticRange);
    query.result = FormatContestRange(query.resultRange.get(), type);
    query.resolved = true;
    query.resultOrigin = origin;
}

bool ShouldRecordContestResult(
    ContestQuery& query, const std::string& result, Type* type, const ValueRange* range,
    bool fromBeforeProgramPoint = false, bool refinesBeforeProgramPoint = false,
    bool aggregatesDeclaredBindingLifetime = false)
{
    if (!aggregatesDeclaredBindingLifetime) {
        if (fromBeforeProgramPoint && query.hasDirectPointLoadResult) {
            return false;
        }
        if (query.hasBeforePointResult && !fromBeforeProgramPoint) {
            if (!refinesBeforeProgramPoint) {
                return false;
            }
            if (!query.hasDirectPointLoadResult) {
                return true;
            }
        }
    }
    if (!query.resolved) {
        return true;
    }
    auto candidate = CloneContestRangeOrTop(type, range);
    auto currentType = query.type == nullptr ? type : query.type;
    auto current = CloneContestRangeOrTop(currentType, query.resultRange.get());
    if (candidate == nullptr || current == nullptr) {
        return false;
    }
    if (!AreContestTypesCompatible(currentType, type) ||
        current->GetRangeKind() != candidate->GetRangeKind()) {
        SetContestQueryResult(query, currentType, nullptr, query.resultOrigin);
        return false;
    }
    if (auto updated = current->Join(*candidate); updated.has_value()) {
        current = std::move(updated.value());
        query.result = FormatContestRange(current.get(), currentType);
        query.resultRange = std::move(current);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisJoin] query=" << query.variableName << '@' << query.line
                      << " candidate=" << result << " joined=" << query.result << '\n';
        }
    }
    return false;
}

const ValueRange* GetContestRangeForValue(const RangeDomain& state, Value* value)
{
    if (value == nullptr) {
        return nullptr;
    }
    if (value->GetType()->IsRef()) {
        if (auto object = state.CheckAbstractObjectRefBy(value); object != nullptr) {
            return state.CheckAbstractValue(object);
        }
    }
    return state.CheckAbstractValue(value);
}

enum class ContestRangeObservationKind : uint8_t {
    ABSENT,
    KNOWN,
    UNKNOWN
};

struct ContestRangeObservation {
    ContestRangeObservationKind kind{ContestRangeObservationKind::ABSENT};
    const ValueRange* range{nullptr};
};

ContestRangeObservation ObserveContestRange(const RangeDomain& state, Value* value)
{
    if (value == nullptr || state.IsBottom()) {
        return {};
    }
    auto classifyValueDomain = [](const RangeValueDomain* domain) {
        if (domain == nullptr || domain->IsBottom()) {
            return ContestRangeObservation{};
        }
        if (domain->IsTop() || domain->GetKind() != RangeValueDomain::ValueKind::VAL) {
            return ContestRangeObservation{ContestRangeObservationKind::UNKNOWN, nullptr};
        }
        auto range = domain->CheckAbsVal();
        return range == nullptr
            ? ContestRangeObservation{ContestRangeObservationKind::UNKNOWN, nullptr}
            : ContestRangeObservation{ContestRangeObservationKind::KNOWN, range};
    };

    auto domain = state.CheckAbstractValueWithTopBottom(value);
    if (!value->GetType()->IsRef()) {
        return classifyValueDomain(domain);
    }
    if (domain == nullptr || domain->IsBottom()) {
        return {};
    }
    if (domain->IsTop() || domain->GetKind() != RangeValueDomain::ValueKind::REF ||
        domain->GetRef()->IsTopRefInstance()) {
        return {ContestRangeObservationKind::UNKNOWN, nullptr};
    }
    auto object = state.CheckAbstractObjectRefBy(value);
    if (object == nullptr) {
        return {};
    }
    if (object->IsTopObjInstance()) {
        return {ContestRangeObservationKind::UNKNOWN, nullptr};
    }
    return classifyValueDomain(state.CheckAbstractValueWithTopBottom(object));
}

const Store* FindContestBindingInitialization(const Debug& debug)
{
    auto binding = debug.GetValue();
    auto definingExpression = GetLocalDefiningExpression(binding);
    auto block = debug.GetParentBlock();
    if (binding == nullptr || block == nullptr || definingExpression == nullptr ||
        definingExpression->GetExprKind() != ExprKind::ALLOCATE ||
        definingExpression->GetParentBlock() != block) {
        return nullptr;
    }
    bool afterDebug = false;
    for (auto expression : block->GetExpressions()) {
        if (expression == &debug) {
            afterDebug = true;
            continue;
        }
        if (!afterDebug) {
            continue;
        }
        if (expression->GetExprKind() == ExprKind::STORE) {
            auto store = StaticCast<const Store*>(expression);
            if (store->GetLocation() == binding) {
                return store;
            }
        }
        const auto& operands = expression->GetOperands();
        if (std::find(operands.begin(), operands.end(), binding) != operands.end()) {
            return nullptr;
        }
    }
    return nullptr;
}

void RememberContestAggregateBinding(
    ContestAggregateMap& aggregates, const Debug& debug, const std::string& fileKey, Type* type)
{
    const auto& location = debug.GetDebugLocation();
    auto variableName = debug.GetSrcCodeIdentifier();
    auto binding = debug.GetValue();
    auto& aggregate = aggregates[MakeContestAggregateKey(fileKey, location.GetBeginPos().line, variableName)];
    aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
    auto existing = std::find_if(aggregate.bindings.begin(), aggregate.bindings.end(),
        [binding](const auto& entry) { return entry.value == binding; });
    if (binding == nullptr || existing != aggregate.bindings.end()) {
        return;
    }
    if (aggregate.bindings.size() >= MAX_CONTEST_AGGREGATE_BINDINGS) {
        aggregate.bindingsOverflow = true;
        return;
    }
    aggregate.bindings.emplace_back(ContestAggregateBinding{binding, FindContestBindingInitialization(debug)});
}

ContestReachability IsBlockInContestCycle(const Block* block)
{
    if (block == nullptr) {
        return ContestReachability::UNREACHABLE;
    }
    bool sawUnknown = false;
    for (auto successor : block->GetSuccessors()) {
        auto reachability = CanReachBlockForQueryMapping(successor, block);
        if (reachability == ContestReachability::REACHABLE) {
            return ContestReachability::REACHABLE;
        }
        sawUnknown = sawUnknown || reachability == ContestReachability::UNKNOWN;
    }
    return sawUnknown ? ContestReachability::UNKNOWN : ContestReachability::UNREACHABLE;
}

struct ContestLoopLifetime {
    Type* type{nullptr};
    std::unique_ptr<ValueRange> range;
    bool hasLoopUse{false};
    bool complete{true};
    bool hasContextObservation{false};
};

struct ContestContextLoopObservation {
    std::unique_ptr<ValueRange> range;
    bool complete{true};
};

using ContestContextLoopObservationMap =
    std::unordered_map<const Expression*, ContestContextLoopObservation>;

void MergeContestLoopLifetime(
    ContestLoopLifetime& lifetime, Type* type, const ValueRange* range)
{
    if (type == nullptr || range == nullptr) {
        lifetime.complete = false;
        return;
    }
    if (lifetime.type == nullptr) {
        lifetime.type = type;
    }
    if (!AreContestTypesCompatible(lifetime.type, type) ||
        (lifetime.range != nullptr && lifetime.range->GetRangeKind() != range->GetRangeKind())) {
        lifetime.complete = false;
        return;
    }
    if (lifetime.range == nullptr) {
        lifetime.range = range->Clone();
    } else if (auto joined = lifetime.range->Join(*range); joined.has_value()) {
        lifetime.range = std::move(joined.value());
    }
}

ContestLoopLifetime CollectContestLoopLifetime(const ContestAggregateBinding& aggregateBinding,
    const ContestContextLoopObservationMap& contextObservations)
{
    ContestLoopLifetime lifetime;
    auto binding = aggregateBinding.value;
    if (binding == nullptr || !binding->GetType()->IsRef()) {
        return lifetime;
    }
    auto type = GetQueryValueType(binding);
    bool escaped = false;
    bool hasLoopStore = false;
    size_t loopUses = 0;
    for (auto user : binding->GetUsers()) {
        if (user == nullptr) {
            escaped = true;
            continue;
        }
        if (user->GetExprKind() == ExprKind::DEBUGEXPR &&
            StaticCast<const Debug*>(user)->GetValue() == binding) {
            continue;
        }
        if (user == aggregateBinding.initializationStore) {
            continue;
        }
        const bool directLoad = user->GetExprKind() == ExprKind::LOAD &&
            StaticCast<const Load*>(user)->GetLocation() == binding;
        const bool directStore = user->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(user)->GetLocation() == binding;
        if (!directLoad && !directStore) {
            escaped = true;
            continue;
        }
        auto cycle = IsBlockInContestCycle(user->GetParentBlock());
        if (cycle == ContestReachability::UNREACHABLE) {
            continue;
        }
        lifetime.hasLoopUse = true;
        hasLoopStore = hasLoopStore || directStore;
        if (cycle == ContestReachability::UNKNOWN) {
            lifetime.complete = false;
            continue;
        }
        if (++loopUses > MAX_CONTEST_LOOP_USES) {
            lifetime.complete = false;
            continue;
        }
        std::unique_ptr<ValueRange> observed;
        auto contextObservation = contextObservations.find(user);
        if (contextObservation != contextObservations.end()) {
            lifetime.hasContextObservation = true;
            if (!contextObservation->second.complete ||
                contextObservation->second.range == nullptr) {
                lifetime.complete = false;
                continue;
            }
            observed = contextObservation->second.range->Clone();
        } else {
            observed = RangeAnalysis::GetBoundedLoopObservedRange(user);
        }
        if (observed == nullptr) {
            lifetime.complete = false;
            continue;
        }
        MergeContestLoopLifetime(lifetime, type, observed.get());
    }
    if (!hasLoopStore) {
        return ContestLoopLifetime{};
    }
    if (lifetime.hasLoopUse && escaped) {
        lifetime.complete = false;
    }
    return lifetime;
}

bool CollectContestContextLoopObservations(
    ContestContextLoopObservationMap& observations, Results<RangeDomain>& result)
{
    constexpr size_t MAX_CONTEXT_LOOP_OBSERVATIONS = 4096;
    auto analysis = dynamic_cast<RangeAnalysis*>(result.GetAnalysis());
    if (analysis == nullptr) {
        return false;
    }
    std::unordered_set<Expression*> visitedExpressions;
    result.VisitWith(
        [&](const RangeDomain&, Expression* expression, size_t) {
            if (expression != nullptr) {
                visitedExpressions.emplace(expression);
            }
        },
        [](const RangeDomain&, Expression*, size_t) {},
        [](const RangeDomain&, Terminator*, std::optional<Block*>) {});
    for (auto expression : visitedExpressions) {
        if (expression->GetExprKind() != ExprKind::LOAD &&
            expression->GetExprKind() != ExprKind::STORE) {
            continue;
        }
        if (IsBlockInContestCycle(expression->GetParentBlock()) !=
            ContestReachability::REACHABLE) {
            continue;
        }
        auto found = observations.find(expression);
        if (found == observations.end()) {
            if (observations.size() >= MAX_CONTEXT_LOOP_OBSERVATIONS) {
                return false;
            }
            found = observations.emplace(
                expression, ContestContextLoopObservation{}).first;
        }
        auto observed = analysis->GetLocalBoundedLoopObservedRange(expression);
        if (observed == nullptr) {
            found->second.complete = false;
            continue;
        }
        if (found->second.range == nullptr) {
            found->second.range = std::move(observed);
        } else if (auto joined = found->second.range->Join(*observed);
            joined.has_value()) {
            found->second.range = std::move(joined.value());
        }
    }
    return true;
}

void ApplyContestAggregates(
    std::vector<ContestQuery>& queries, const ContestAggregateMap& aggregates,
    const ContestContextLoopObservationMap& contextObservations)
{
    for (auto& query : queries) {
        auto it = aggregates.find(MakeContestAggregateKey(query.fileKey, query.line, query.variableName));
        if (it == aggregates.end()) {
            continue;
        }
        ContestLoopLifetime combined;
        bool sawNonLoopBinding = false;
        if (it->second.bindingsOverflow) {
            combined.hasLoopUse = true;
            combined.complete = false;
        }
        for (const auto& binding : it->second.bindings) {
            auto lifetime = CollectContestLoopLifetime(binding, contextObservations);
            if (!lifetime.hasLoopUse) {
                sawNonLoopBinding = true;
                continue;
            }
            if (sawNonLoopBinding) {
                combined.complete = false;
            }
            combined.hasLoopUse = true;
            combined.complete = combined.complete && lifetime.complete;
            combined.hasContextObservation =
                combined.hasContextObservation || lifetime.hasContextObservation;
            if (lifetime.range != nullptr) {
                MergeContestLoopLifetime(combined, lifetime.type, lifetime.range.get());
            }
        }
        if (!combined.hasLoopUse) {
            continue;
        }
        if (sawNonLoopBinding && it->second.bindings.size() > 1) {
            combined.complete = false;
        }
        auto type = query.type == nullptr ? it->second.type : query.type;
        if (type == nullptr || (!type->IsInteger() && !type->IsBoolean())) {
            continue;
        }
        if (!AreContestTypesCompatible(type, combined.type) && combined.range != nullptr) {
            combined.complete = false;
        }
        if (query.hasUnknownPointObservation || combined.range == nullptr) {
            combined.complete = false;
        }
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisAggregate] query=" << query.variableName << '@' << query.line
                      << " complete=" << combined.complete
                      << " range=" << FormatContestRange(combined.range.get(), type)
                      << " old=" << query.result << '\n';
        }
        auto aggregateRange = combined.complete ? combined.range.get() : nullptr;
        if (query.boundGlobal != nullptr && query.resolved &&
            query.resultRange != nullptr) {
            if (aggregateRange == nullptr ||
                !AreContestTypesCompatible(type, query.type) ||
                query.resultRange->GetRangeKind() !=
                    aggregateRange->GetRangeKind()) {
                // The abstract-memory/context result already represents the
                // global lifetime. An incomplete loop-only observation cannot
                // safely replace it.
                continue;
            }
            auto joined = query.resultRange->Clone();
            if (auto updated = joined->Join(*aggregateRange);
                updated.has_value()) {
                joined = std::move(updated.value());
            }
            SetContestQueryResult(
                query, type, joined.get(), query.resultOrigin);
            continue;
        }
        if (query.resolved && query.resultOrigin == ContestResultOrigin::CONTEXT_SUMMARY &&
            !(combined.complete && combined.hasContextObservation &&
                aggregateRange != nullptr)) {
            // The context candidate already aggregates every reachable concrete
            // invocation. The context-insensitive root state is strictly weaker
            // for parameter-derived loop bindings and must not widen it to Top.
            continue;
        }
        SetContestQueryResult(query, type, aggregateRange, ContestResultOrigin::CHIR_ANALYSIS);
    }
}

// 判断查询位置是否匹配 CHIR 调试位置信息。
bool IsSameQueryLocation(const ContestQuery& query, const DebugLocation& location, const std::filesystem::path& contestRoot)
{
    if (query.line != location.GetBeginPos().line) {
        return false;
    }
    auto locationKey = GetLocationFileKey(location, contestRoot);
    if (query.fileKey == locationKey) {
        return true;
    }
    return !HasDirectoryPart(query.sourceFileName) && query.fileName == BaseName(location.GetFileName());
}

bool IsSameQueryFile(const ContestQuery& query, const DebugLocation& location,
    const std::filesystem::path& contestRoot)
{
    auto locationKey = GetLocationFileKey(location, contestRoot);
    if (query.fileKey == locationKey) {
        return true;
    }
    return !HasDirectoryPart(query.sourceFileName) && query.fileName == BaseName(location.GetFileName());
}

GlobalVar* GetDirectGlobalLocation(const Expression& expression);

void BindGlobalQueries(const Ptr<const Package>& package, std::vector<ContestQuery>& queries,
    const std::filesystem::path& contestRoot)
{
    for (auto& query : queries) {
        GlobalVar* matchedGlobal = nullptr;
        bool matchedDeclaration = false;
        bool ambiguous = false;
        for (auto global : package->GetGlobalVars()) {
            if (!IsGlobalVarInCurrentPackage(global) || query.variableName != global->GetSrcCodeIdentifier() ||
                !IsSameQueryLocation(query, global->GetDebugLocation(), contestRoot)) {
                continue;
            }
            if (matchedGlobal != nullptr && matchedGlobal != global) {
                ambiguous = true;
                break;
            }
            matchedGlobal = global;
            matchedDeclaration = true;
        }
        if (!ambiguous && matchedGlobal == nullptr) {
            for (auto func : package->GetGlobalFuncsWithBody()) {
                if (func == nullptr || func->GetBody() == nullptr) {
                    continue;
                }
                for (auto block : func->GetBody()->GetAllBlocks()) {
                    for (auto expression : block->GetExpressions()) {
                        auto global = GetDirectGlobalLocation(*expression);
                        if (global == nullptr || query.variableName != global->GetSrcCodeIdentifier() ||
                            !IsSameQueryLocation(query, expression->GetDebugLocation(), contestRoot)) {
                            continue;
                        }
                        if (matchedGlobal != nullptr && matchedGlobal != global) {
                            ambiguous = true;
                            break;
                        }
                        matchedGlobal = global;
                        matchedDeclaration = false;
                    }
                    if (ambiguous) {
                        break;
                    }
                }
                if (ambiguous) {
                    break;
                }
            }
        }
        if (ambiguous || matchedGlobal == nullptr) {
            continue;
        }
        query.boundGlobal = matchedGlobal;
        query.isGlobalDeclarationQuery = matchedDeclaration;
        query.type = GetQueryValueType(matchedGlobal);
        query.typeHint = GetQueryTypeHint(query.type);
    }
}

bool HasPotentialLocalBindingBeforeQuery(const Ptr<const Package>& package, const ContestQuery& query,
    const std::filesystem::path& contestRoot)
{
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (func == nullptr || func->GetBody() == nullptr) {
            continue;
        }
        for (auto block : func->GetBody()->GetAllBlocks()) {
            for (auto expression : block->GetExpressions()) {
                if (expression->GetExprKind() != ExprKind::DEBUGEXPR) {
                    continue;
                }
                auto debug = StaticCast<const Debug*>(expression);
                auto value = debug->GetValue();
                if (value == nullptr || IsGlobalVarInCurrentPackage(value) ||
                    debug->GetSrcCodeIdentifier() != query.variableName ||
                    debug->GetDebugLocation().GetBeginPos().line > query.line ||
                    !IsSameQueryFile(query, debug->GetDebugLocation(), contestRoot)) {
                    continue;
                }
                return true;
            }
        }
    }
    return false;
}

void BindUnambiguousGlobalQueries(const Ptr<const Package>& package, std::vector<ContestQuery>& queries,
    const std::filesystem::path& contestRoot)
{
    for (auto& query : queries) {
        if (!query.valid || query.boundGlobal != nullptr ||
            HasPotentialLocalBindingBeforeQuery(package, query, contestRoot)) {
            continue;
        }
        GlobalVar* matchedGlobal = nullptr;
        bool ambiguous = false;
        for (auto global : package->GetGlobalVars()) {
            if (!IsGlobalVarInCurrentPackage(global) ||
                query.variableName != global->GetSrcCodeIdentifier() ||
                !IsSameQueryFile(query, global->GetDebugLocation(), contestRoot)) {
                continue;
            }
            if (matchedGlobal != nullptr && matchedGlobal != global) {
                ambiguous = true;
                break;
            }
            matchedGlobal = global;
        }
        if (ambiguous || matchedGlobal == nullptr) {
            continue;
        }
        query.boundGlobal = matchedGlobal;
        query.isGlobalDeclarationQuery = false;
        query.type = GetQueryValueType(matchedGlobal);
        query.typeHint = GetQueryTypeHint(query.type);
    }
}

bool MayMatchAnyContestQuery(
    const std::vector<ContestQuery>& queries, const DebugLocation& location, const std::filesystem::path& contestRoot)
{
    return std::any_of(queries.begin(), queries.end(), [&](const auto& query) {
        return query.valid && IsSameQueryLocation(query, location, contestRoot);
    });
}

bool FunctionMayContainContestQuery(
    const Function* func, const std::vector<ContestQuery>& queries, const std::filesystem::path& contestRoot)
{
    if (func == nullptr || func->GetBody() == nullptr) {
        return false;
    }
    for (auto block : func->GetBody()->GetAllBlocks()) {
        for (auto expr : block->GetExpressions()) {
            if (MayMatchAnyContestQuery(queries, expr->GetDebugLocation(), contestRoot)) {
                return true;
            }
        }
        if (auto terminator = block->GetTerminator();
            terminator != nullptr && MayMatchAnyContestQuery(queries, terminator->GetDebugLocation(), contestRoot)) {
            return true;
        }
    }
    return false;
}

// 判断某个值是否已关联指定源码变量名。
using ReverseContestCallGraph = std::unordered_map<const Function*, std::vector<const Function*>>;

void AddReverseContestCallGraphEdge(
    const Function* caller, Value* calleeValue, ReverseContestCallGraph& reverseCallGraph)
{
    if (caller == nullptr || calleeValue == nullptr || !calleeValue->IsFuncWithBody()) {
        return;
    }
    auto callee = StaticCast<const Function*>(calleeValue);
    if (callee == nullptr || callee->GetBody() == nullptr) {
        return;
    }
    auto& callers = reverseCallGraph[callee];
    if (std::find(callers.begin(), callers.end(), caller) == callers.end()) {
        callers.emplace_back(caller);
    }
}

void RecordContestCallExpression(
    const Function* caller, const Expression* expr, ReverseContestCallGraph& reverseCallGraph)
{
    if (expr == nullptr) {
        return;
    }
    if (expr->GetExprKind() == ExprKind::APPLY) {
        AddReverseContestCallGraphEdge(caller, StaticCast<const Apply*>(expr)->GetCallee(), reverseCallGraph);
    } else if (expr->GetExprKind() == ExprKind::APPLY_WITH_EXCEPTION) {
        AddReverseContestCallGraphEdge(
            caller, StaticCast<const ApplyWithException*>(expr)->GetCallee(), reverseCallGraph);
    }
}

ReverseContestCallGraph BuildReverseContestCallGraph(const Ptr<const Package>& package)
{
    ReverseContestCallGraph reverseCallGraph;
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (func == nullptr || func->GetBody() == nullptr) {
            continue;
        }
        for (auto block : func->GetBody()->GetAllBlocks()) {
            for (auto expr : block->GetExpressions()) {
                RecordContestCallExpression(func, expr, reverseCallGraph);
            }
            RecordContestCallExpression(func, block->GetTerminator(), reverseCallGraph);
        }
    }
    return reverseCallGraph;
}

std::unordered_set<const Function*> CollectContestRelevantFunctions(
    const Ptr<const Package>& package, const std::vector<ContestQuery>& queries, const std::filesystem::path& contestRoot)
{
    constexpr size_t MAX_CONTEST_CONTEXT_CALL_DEPTH = 6;
    constexpr size_t MAX_CONTEST_RELEVANT_FUNCTIONS = 64;

    std::unordered_set<const Function*> relevantFunctions;
    std::vector<std::pair<const Function*, size_t>> worklist;
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (FunctionMayContainContestQuery(func, queries, contestRoot) &&
            relevantFunctions.emplace(func).second) {
            worklist.emplace_back(func, 0);
        }
    }
    const bool hasValidQuery = std::any_of(queries.begin(), queries.end(), [](const auto& query) {
        return query.valid;
    });
    const bool hasBoundGlobalQuery = std::any_of(queries.begin(), queries.end(), [](const auto& query) {
        return query.valid && query.boundGlobal != nullptr;
    });
    if (hasValidQuery) {
        for (auto func : package->GetGlobalFuncsWithBody()) {
            if (func != nullptr && func->GetBody() != nullptr && func->GetFuncKind() == FuncKind::MAIN_ENTRY &&
                relevantFunctions.emplace(func).second) {
                worklist.emplace_back(func, 0);
            }
        }
    }
    if (hasBoundGlobalQuery) {
        auto packageInit = package->GetPackageInitFunc();
        if (packageInit != nullptr && packageInit->GetBody() != nullptr &&
            relevantFunctions.emplace(packageInit).second) {
            worklist.emplace_back(packageInit, 0);
        }
    }
    if (worklist.empty()) {
        return relevantFunctions;
    }

    auto reverseCallGraph = BuildReverseContestCallGraph(package);
    for (size_t index = 0; index < worklist.size(); ++index) {
        auto [func, depth] = worklist[index];
        if (depth >= MAX_CONTEST_CONTEXT_CALL_DEPTH) {
            continue;
        }
        auto callers = reverseCallGraph.find(func);
        if (callers == reverseCallGraph.end()) {
            continue;
        }
        for (auto caller : callers->second) {
            if (relevantFunctions.size() >= MAX_CONTEST_RELEVANT_FUNCTIONS) {
                return relevantFunctions;
            }
            if (relevantFunctions.emplace(caller).second) {
                worklist.emplace_back(caller, depth + 1);
            }
        }
    }
    return relevantFunctions;
}

bool HasValueNameForQuery(const ValueNameMap& valueNames, Value* value, const ContestQuery& query)
{
    auto it = valueNames.find(value);
    if (it == valueNames.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [&query](const auto& info) {
        if (info.name != query.variableName) {
            return false;
        }
        if (info.fileKey == query.fileKey) {
            return true;
        }
        return !HasDirectoryPart(query.sourceFileName) && query.fileName == BaseName(info.fileKey);
    });
}

// 记录 Debug(value, name) 映射，供后续 operand 查询解析使用。
void RememberValueName(ValueNameMap& valueNames, const Debug& debug, const std::filesystem::path& contestRoot)
{
    auto value = debug.GetValue();
    auto& names = valueNames[value];
    ValueNameInfo info{debug.GetSrcCodeIdentifier(), GetLocationFileKey(debug.GetDebugLocation(), contestRoot),
        debug.GetDebugLocation().GetBeginPos().line, debug.GetDebugLocation().GetScopeInfo()};
    auto sameInfo = [&info](const auto& old) {
        return old.name == info.name && old.fileKey == info.fileKey && old.line == info.line &&
            old.scopeInfo == info.scopeInfo;
    };
    if (std::find_if(names.begin(), names.end(), sameInfo) == names.end()) {
        names.emplace_back(std::move(info));
    }
}

// 记录查询类型，即使精确程序点解析失败也能正确 fallback。
void RememberQueryType(std::vector<ContestQuery>& queries, const Debug& debug, const std::filesystem::path& contestRoot)
{
    auto type = GetQueryValueType(debug.GetValue());
    auto typeHint = GetQueryTypeHint(type);
    auto fileKey = GetLocationFileKey(debug.GetDebugLocation(), contestRoot);
    for (auto& query : queries) {
        if (!query.valid || query.type || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (query.typeHint != ContestQueryTypeHint::UNKNOWN && query.typeHint != typeHint) {
            continue;
        }
        if (query.fileKey == fileKey ||
            (!HasDirectoryPart(query.sourceFileName) && query.fileName == BaseName(debug.GetDebugLocation().GetFileName()))) {
            query.type = type;
            query.typeHint = typeHint;
        }
    }
}

bool IsScopePrefix(const std::vector<int>& declarationScope, const std::vector<int>& useScope)
{
    return declarationScope.size() <= useScope.size() &&
        std::equal(declarationScope.begin(), declarationScope.end(), useScope.begin());
}

bool ValueNameInfoMatchesQuery(const ValueNameInfo& info, const ContestQuery& query,
    const std::vector<int>& useScope)
{
    if (info.name != query.variableName || info.line > query.line || !IsScopePrefix(info.scopeInfo, useScope)) {
        return false;
    }
    if (info.fileKey == query.fileKey) {
        return true;
    }
    return !HasDirectoryPart(query.sourceFileName) && query.fileName == BaseName(info.fileKey);
}

std::vector<Value*> FindVisibleNamedValues(
    const ContestQuery& query, const ValueNameMap& valueNames, const std::vector<int>& useScope)
{
    const bool traceMapping = std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr;
    if (traceMapping) {
        std::cerr << "[RangeAnalysisScope] query=" << query.variableName << '@' << query.line << " useScope=";
        for (auto scope : useScope) {
            std::cerr << scope << ',';
        }
        std::cerr << '\n';
    }
    size_t bestScopeDepth = 0;
    unsigned bestLine = 0;
    std::vector<Value*> candidates;
    for (const auto& [value, names] : valueNames) {
        for (const auto& info : names) {
            if (traceMapping && info.name == query.variableName) {
                std::cerr << "[RangeAnalysisScope] declaration file=" << info.fileKey << " line=" << info.line
                          << " scope=";
                for (auto scope : info.scopeInfo) {
                    std::cerr << scope << ',';
                }
                std::cerr << " visible=" << ValueNameInfoMatchesQuery(info, query, useScope) << '\n';
            }
            if (!ValueNameInfoMatchesQuery(info, query, useScope)) {
                continue;
            }
            if (info.scopeInfo.size() < bestScopeDepth ||
                (info.scopeInfo.size() == bestScopeDepth && info.line < bestLine)) {
                continue;
            }
            if (info.scopeInfo.size() > bestScopeDepth || info.line > bestLine) {
                candidates.clear();
                bestScopeDepth = info.scopeInfo.size();
                bestLine = info.line;
            }
            if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
                candidates.emplace_back(value);
            }
        }
    }
    if (traceMapping) {
        std::cerr << "[RangeAnalysisScope] candidates=" << candidates.size() << '\n';
    }
    return candidates;
}

Expression* GetLocalDefiningExpression(Value* value)
{
    if (value == nullptr || !value->IsLocalVar()) {
        return nullptr;
    }
    return StaticCast<LocalVar*>(value)->GetExpr();
}

std::unique_ptr<ValueRange> GetBoundedLoopObservedRangeForValue(Value* value)
{
    auto expression = GetLocalDefiningExpression(value);
    return expression == nullptr ? nullptr : RangeAnalysis::GetBoundedLoopObservedRange(expression);
}

bool IsLoadFromLocation(Value* value, Value* location)
{
    auto expression = GetLocalDefiningExpression(value);
    return expression != nullptr && expression->GetExprKind() == ExprKind::LOAD &&
        StaticCast<const Load*>(expression)->GetLocation() == location;
}

ContestReachability CanReachBlockForQueryMapping(const Block* start, const Block* target)
{
    constexpr size_t MAX_QUERY_CFG_BLOCKS = 4096;
    if (start == nullptr || target == nullptr) {
        return ContestReachability::UNREACHABLE;
    }
    std::vector<const Block*> worklist{start};
    std::unordered_set<const Block*> scheduled{start};
    while (!worklist.empty()) {
        auto block = worklist.back();
        worklist.pop_back();
        if (block == target) {
            return ContestReachability::REACHABLE;
        }
        for (auto successor : block->GetSuccessors()) {
            if (successor == nullptr || scheduled.find(successor) != scheduled.end()) {
                continue;
            }
            if (scheduled.size() >= MAX_QUERY_CFG_BLOCKS) {
                return ContestReachability::UNKNOWN;
            }
            scheduled.emplace(successor);
            worklist.emplace_back(successor);
        }
    }
    return ContestReachability::UNREACHABLE;
}

bool IsUnnamedLoopInductionLoad(const Expression& expression)
{
    if (expression.GetExprKind() != ExprKind::LOAD || expression.GetResult() == nullptr ||
        !expression.GetResult()->GetType()->IsInteger()) {
        return false;
    }
    auto load = StaticCast<const Load*>(&expression);
    auto location = load->GetLocation();
    if (location == nullptr || !location->GetType()->IsRef()) {
        return false;
    }
    auto rootType = StaticCast<RefType*>(location->GetType())->GetRootBaseType();
    if (rootType == nullptr || !rootType->IsInteger()) {
        return false;
    }
    auto loadBlock = expression.GetParentBlock();
    for (auto user : location->GetUsers()) {
        if (user->GetExprKind() != ExprKind::STORE ||
            StaticCast<const Store*>(user)->GetLocation() != location) {
            continue;
        }
        auto storedExpression = GetLocalDefiningExpression(StaticCast<const Store*>(user)->GetValue());
        if (storedExpression == nullptr || storedExpression->GetExprMajorKind() != ExprMajorKind::BINARY_EXPR ||
            (storedExpression->GetExprKind() != ExprKind::ADD && storedExpression->GetExprKind() != ExprKind::SUB)) {
            continue;
        }
        auto binary = StaticCast<const BinaryExpression*>(storedExpression);
        const bool readsLocation = IsLoadFromLocation(binary->GetLHSOperand(), location) ||
            (storedExpression->GetExprKind() == ExprKind::ADD &&
                IsLoadFromLocation(binary->GetRHSOperand(), location));
        auto storeBlock = user->GetParentBlock();
        auto reachesStore = CanReachBlockForQueryMapping(loadBlock, storeBlock);
        auto reachesLoad = CanReachBlockForQueryMapping(storeBlock, loadBlock);
        if (readsLocation && reachesStore == ContestReachability::REACHABLE &&
            reachesLoad == ContestReachability::REACHABLE) {
            return true;
        }
    }
    return false;
}

bool HasVisibleNamedValueAtLocation(
    const ContestQuery& query, const ValueNameMap& valueNames, const DebugLocation& location)
{
    return std::any_of(valueNames.begin(), valueNames.end(), [&](const auto& entry) {
        return std::any_of(entry.second.begin(), entry.second.end(), [&](const auto& info) {
            return ValueNameInfoMatchesQuery(info, query, location.GetScopeInfo());
        });
    });
}

std::optional<size_t> FindUniqueUnnamedQueryAtLocation(const std::vector<ContestQuery>& queries,
    const ValueNameMap& valueNames, const DebugLocation& location, const std::filesystem::path& contestRoot)
{
    std::optional<size_t> match;
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid || !IsSameQueryLocation(query, location, contestRoot) ||
            HasVisibleNamedValueAtLocation(query, valueNames, location)) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = index;
    }
    return match;
}

void ResolveQueryAtUnnamedLoopInductionLoad(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const Expression& expression, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    if (!IsUnnamedLoopInductionLoad(expression)) {
        return;
    }
    auto queryIndex = FindUniqueUnnamedQueryAtLocation(
        queries, valueNames, expression.GetDebugLocation(), contestRoot);
    if (!queryIndex.has_value()) {
        return;
    }
    auto& query = queries[*queryIndex];
    auto value = expression.GetResult();
    auto type = GetQueryValueType(value);
    auto range = GetContestRangeForValue(state, value);
    auto observedRange = RangeAnalysis::GetBoundedLoopObservedRange(&expression);
    query.hasBoundedPointResult = query.hasBoundedPointResult || observedRange != nullptr;
    if (observedRange != nullptr && IsContestTopRange(range, type)) {
        range = observedRange.get();
    }
    auto result = FormatContestRange(range, type);
    if (!ShouldRecordContestResult(query, result, type, range)) {
        return;
    }
    SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
}

void ResolveQueriesFromVisibleNames(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const DebugLocation& location, const RangeDomain& state, const std::filesystem::path& contestRoot,
    bool beforeProgramPoint = false)
{
    auto useScope = location.GetScopeInfo();
    for (auto& query : queries) {
        if (!query.valid || !IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        if (!beforeProgramPoint && query.hasBoundedLifetimeResult) {
            continue;
        }
        auto candidates = FindVisibleNamedValues(query, valueNames, useScope);
        Type* type = nullptr;
        std::unique_ptr<ValueRange> joined;
        for (auto value : candidates) {
            auto candidateType = GetQueryValueType(value);
            auto range = GetContestRangeForValue(state, value);
            if (candidateType == nullptr) {
                continue;
            }
            if (query.typeHint != ContestQueryTypeHint::UNKNOWN &&
                query.typeHint != GetQueryTypeHint(candidateType)) {
                continue;
            }
            if (value->IsParameter() && IsContestTopRange(range, candidateType)) {
                query.hasUnknownPointObservation = true;
            }
            if (range == nullptr) {
                continue;
            }
            type = type == nullptr ? candidateType : type;
            if (joined == nullptr) {
                joined = range->Clone();
            } else if (joined->GetRangeKind() == range->GetRangeKind()) {
                if (auto merged = joined->Join(*range); merged.has_value()) {
                    joined = std::move(merged.value());
                }
            }
        }
        if (type == nullptr || joined == nullptr) {
            continue;
        }
        auto result = FormatContestRange(joined.get(), type);
        const bool shouldRecord =
            ShouldRecordContestResult(query, result, type, joined.get(), beforeProgramPoint);
        if (beforeProgramPoint) {
            query.hasBeforePointResult = true;
        }
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, joined.get(), ContestResultOrigin::CHIR_ANALYSIS);
    }
}

void ResolveQueryAtDebug(std::vector<ContestQuery>& queries, ValueNameMap& valueNames,
    ContestAggregateMap& aggregates, const Debug& debug, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    RememberValueName(valueNames, debug, contestRoot);
    RememberQueryType(queries, debug, contestRoot);
    auto fileKey = GetLocationFileKey(debug.GetDebugLocation(), contestRoot);
    RememberContestAggregateBinding(aggregates, debug, fileKey, GetQueryValueType(debug.GetValue()));
    if (!debug.GetValue()->GetType()->IsRef()) {
        auto type = GetQueryValueType(debug.GetValue());
        auto observed = GetBoundedLoopObservedRangeForValue(debug.GetValue());
        auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, debug.GetValue());
        for (auto& query : queries) {
            if (!query.valid || query.variableName != debug.GetSrcCodeIdentifier()) {
                continue;
            }
            if (!IsSameQueryLocation(query, debug.GetDebugLocation(), contestRoot)) {
                continue;
            }
            query.hasBoundedPointResult = query.hasBoundedPointResult || observed != nullptr;
            if (debug.GetValue()->IsParameter() && IsContestTopRange(range, type)) {
                query.hasUnknownPointObservation = true;
            }
            auto result = FormatContestRange(range, type);
            if (!ShouldRecordContestResult(query, result, type, range)) {
                continue;
            }
            SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
        }
    }
    ResolveQueriesFromVisibleNames(queries, valueNames, debug.GetDebugLocation(), state, contestRoot);
}

// 通过已记录的 value-name 映射解析同源码行 operand 查询。
void ResolveQueryAtValue(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames, const DebugLocation& location,
    Value* value, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    for (auto& query : queries) {
        if (!query.valid || !HasValueNameForQuery(valueNames, value, query)) {
            continue;
        }
        if (!IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        auto type = GetQueryValueType(value);
        auto range = GetContestRangeForValue(state, value);
        if (value->IsParameter() && IsContestTopRange(range, type)) {
            query.hasUnknownPointObservation = true;
        }
        auto result = FormatContestRange(range, type);
        if (!ShouldRecordContestResult(query, result, type, range)) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
    }
}

// 在非 Debug 表达式上尝试解析所有 operand 形式查询。
GlobalVar* GetDirectGlobalLocation(const Expression& expression)
{
    Value* location = nullptr;
    if (expression.GetExprKind() == ExprKind::LOAD) {
        location = StaticCast<const Load*>(&expression)->GetLocation();
    } else if (expression.GetExprKind() == ExprKind::STORE) {
        location = StaticCast<const Store*>(&expression)->GetLocation();
    }
    return IsGlobalVarInCurrentPackage(location) ? DynamicCast<GlobalVar*>(location) : nullptr;
}

void ResolveQueryBeforeGlobalAccess(std::vector<ContestQuery>& queries, const Expression& expression,
    const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    auto global = GetDirectGlobalLocation(expression);
    if (global == nullptr) {
        return;
    }
    auto type = GetQueryValueType(global);
    auto range = GetContestRangeForValue(state, global);
    for (auto& query : queries) {
        if (!query.valid || query.variableName != global->GetSrcCodeIdentifier() ||
            !IsSameQueryLocation(query, expression.GetDebugLocation(), contestRoot)) {
            continue;
        }
        auto result = FormatContestRange(range, type);
        const bool shouldRecord = ShouldRecordContestResult(
            query, result, type, range, /* fromBeforeProgramPoint = */ true);
        query.hasBeforePointResult = true;
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
    }
}

void MergeContestContextCandidate(
    ContestContextCandidateMap& candidates, size_t queryIndex, Type* type, const ValueRange* range,
    bool unknownObservation = false, bool auxiliary = false)
{
    if (type == nullptr || (range == nullptr && !unknownObservation)) {
        return;
    }
    auto& candidate = candidates[queryIndex];
    const bool hadObservation = candidate.range != nullptr || candidate.hasUnknownObservation;
    const bool wasAuxiliaryOnly = candidate.auxiliaryOnly;
    if (candidate.type == nullptr) {
        candidate.type = type;
    }
    if (!AreContestTypesCompatible(candidate.type, type)) {
        candidate.range = MakeContestTopRange(candidate.type);
        candidate.incompleteGlobalLifetime = true;
        candidate.hasUnknownObservation = true;
        candidate.auxiliaryOnly = hadObservation ? wasAuxiliaryOnly && auxiliary : auxiliary;
        return;
    }
    const bool incomingUnknown = unknownObservation;
    // Visible-name observations are only a query-mapping aid.  A declaration's
    // allocated object is Top before its initializer Store, so an authoritative
    // Debug/operand observation may replace an auxiliary Top.
    candidate.hasUnknownObservation =
        candidate.hasUnknownObservation || (incomingUnknown && !auxiliary);
    candidate.auxiliaryOnly = hadObservation ? wasAuxiliaryOnly && auxiliary : auxiliary;
    if (candidate.hasUnknownObservation) {
        candidate.range = MakeContestTopRange(candidate.type);
        return;
    }
    auto incoming = CloneContestRangeOrTop(type, range);
    if (incoming == nullptr) {
        return;
    }
    if (candidate.range == nullptr) {
        candidate.range = std::move(incoming);
        return;
    }
    if (auxiliary && !wasAuxiliaryOnly) {
        return;
    }
    if (!auxiliary && wasAuxiliaryOnly) {
        candidate.range = std::move(incoming);
        return;
    }
    if (candidate.range->GetRangeKind() != incoming->GetRangeKind()) {
        candidate.range = MakeContestTopRange(candidate.type);
        candidate.incompleteGlobalLifetime = true;
        return;
    }
    if (auto joined = candidate.range->Join(*incoming); joined.has_value()) {
        candidate.range = std::move(joined.value());
    }
}

void CollectContextCandidateAtGlobalAccess(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const Expression& expression, const RangeDomain& state,
    const std::filesystem::path& contestRoot, bool boundDeclarationsOnly = false)
{
    auto global = GetDirectGlobalLocation(expression);
    if (global == nullptr || state.IsBottom()) {
        return;
    }
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid) {
            continue;
        }
        const bool isBoundDeclaration =
            query.isGlobalDeclarationQuery && query.boundGlobal == global;
        const bool matchesPointGlobal = query.boundGlobal == global ||
            (query.boundGlobal == nullptr &&
                query.variableName == global->GetSrcCodeIdentifier());
        const bool isPointQuery = !boundDeclarationsOnly &&
            !query.isGlobalDeclarationQuery && matchesPointGlobal &&
            IsSameQueryLocation(query, expression.GetDebugLocation(), contestRoot);
        if (!isBoundDeclaration && !isPointQuery) {
            continue;
        }
        if (isBoundDeclaration) {
            auto& candidate = candidates[index];
            candidate.fromGlobalAccess = true;
            if (expression.GetExprKind() != ExprKind::STORE) {
                continue;
            }
            if (expression.GetTopLevelFunc() == global->GetInitFunc()) {
                candidate.sawInitializerObservation = true;
            }
            auto storedValue = StaticCast<const Store*>(&expression)->GetValue();
            auto observed = RangeAnalysis::GetBoundedLoopObservedRange(&expression);
            auto type = GetQueryValueType(global);
            auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, storedValue);
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                auto topLevelFunc = expression.GetTopLevelFunc();
                std::cerr << "[RangeAnalysisGlobalLifetime] kind=store query="
                          << query.variableName << '@' << query.line
                          << " line=" << expression.GetDebugLocation().GetBeginPos().line
                          << " initializer=" << (topLevelFunc == global->GetInitFunc())
                          << " func=" << (topLevelFunc == nullptr ? "<null>" : topLevelFunc->GetIdentifier())
                          << " range=" << FormatContestRange(range, type)
                          << " bounded=" << (observed != nullptr) << '\n';
            }
            if (range == nullptr) {
                candidate.incompleteGlobalLifetime = true;
            }
            MergeContestContextCandidate(candidates, index, type, range, range == nullptr);
            continue;
        }
        candidates[index].fromGlobalAccess = true;
        auto type = GetQueryValueType(global);
        auto range = GetContestRangeForValue(state, global);
        MergeContestContextCandidate(candidates, index, type, range, range == nullptr);
    }
}

void SeedBoundGlobalInitializers(
    const std::vector<ContestQuery>& queries, ContestContextCandidateMap& candidates)
{
    for (size_t index = 0; index < queries.size(); ++index) {
        auto global = queries[index].boundGlobal;
        if (global == nullptr || !queries[index].isGlobalDeclarationQuery) {
            continue;
        }
        auto& candidate = candidates[index];
        candidate.fromGlobalAccess = true;
        candidate.requiresInitializerObservation = true;
        auto initializer = global->GetInitializer();
        if (initializer == nullptr || initializer->IsNullLiteral()) {
            continue;
        }
        auto initialValue = HandleNonNullLiteralValue<RangeValueDomain>(initializer);
        auto range = initialValue.CheckAbsVal();
        if (range == nullptr) {
            continue;
        }
        candidate.sawInitializerObservation = true;
        MergeContestContextCandidate(candidates, index, GetQueryValueType(global), range);
    }
}

void CollectContextCandidateAtGlobalExit(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const Terminator& terminator,
    const RangeDomain& state)
{
    if (terminator.GetExprKind() != ExprKind::EXIT || state.IsBottom()) {
        return;
    }
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        auto global = query.boundGlobal;
        if (!query.valid || !query.isGlobalDeclarationQuery || global == nullptr) {
            continue;
        }
        auto& candidate = candidates[index];
        candidate.fromGlobalAccess = true;
        candidate.sawExitObservation = true;
        auto type = GetQueryValueType(global);
        auto range = GetContestRangeForValue(state, global);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            auto topLevelFunc = terminator.GetTopLevelFunc();
            std::cerr << "[RangeAnalysisGlobalLifetime] kind=exit query="
                      << query.variableName << '@' << query.line
                      << " func=" << (topLevelFunc == nullptr ? "<null>" : topLevelFunc->GetIdentifier())
                      << " range=" << FormatContestRange(range, type) << '\n';
        }
        if (range == nullptr) {
            candidate.incompleteGlobalLifetime = true;
        }
        MergeContestContextCandidate(
            candidates, index, type, range, range == nullptr);
    }
}

void FinalizeBoundGlobalCandidateCompleteness(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, bool requireExitObservation)
{
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid || !query.isGlobalDeclarationQuery ||
            query.boundGlobal == nullptr) {
            continue;
        }
        auto found = candidates.find(index);
        if (found == candidates.end()) {
            continue;
        }
        auto& candidate = found->second;
        if ((candidate.requiresInitializerObservation &&
                !candidate.sawInitializerObservation) ||
            (requireExitObservation && !candidate.sawExitObservation)) {
            candidate.incompleteGlobalLifetime = true;
        }
    }
}

bool IsQueryDeclarationBinding(const ContestQuery& query, const ValueNameMap& valueNames,
    Value* location, const DebugLocation& useLocation)
{
    auto names = valueNames.find(location);
    if (names == valueNames.end()) {
        return false;
    }
    if (useLocation.GetBeginPos().line < query.line) {
        return false;
    }
    const auto& useScope = useLocation.GetScopeInfo();
    return std::any_of(names->second.begin(), names->second.end(), [&](const auto& info) {
        const bool sameFile = info.fileKey == query.fileKey ||
            (!HasDirectoryPart(query.sourceFileName) && BaseName(info.fileKey) == query.fileName);
        return sameFile && info.name == query.variableName && info.line == query.line &&
            IsScopePrefix(info.scopeInfo, useScope);
    });
}

bool IsQueryDeclarationBindingLoad(
    const ContestQuery& query, const ValueNameMap& valueNames, const Load& load)
{
    return IsQueryDeclarationBinding(
        query, valueNames, load.GetLocation(), load.GetDebugLocation());
}

bool IsSafeForwardQueryLoad(const ContestQuery& query, const ValueNameMap& valueNames, const Load& load,
    const std::filesystem::path& contestRoot)
{
    const auto& location = load.GetDebugLocation();
    if (!IsSameQueryFile(query, location, contestRoot)) {
        return false;
    }
    return location.GetBeginPos().line == query.line ||
        IsQueryDeclarationBindingLoad(query, valueNames, load);
}

void ResolveQueryAtLoadResult(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const Expression& expr, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    if (expr.GetExprKind() != ExprKind::LOAD) {
        return;
    }
    auto load = StaticCast<const Load*>(&expr);
    for (auto& query : queries) {
        const bool namedLocation = query.valid && (HasValueNameForQuery(valueNames, load->GetLocation(), query) ||
            (IsGlobalVarInCurrentPackage(load->GetLocation()) &&
                query.variableName == load->GetLocation()->GetSrcCodeIdentifier()));
        const bool safeObservation = namedLocation && IsSafeForwardQueryLoad(query, valueNames, *load, contestRoot);
        const bool lifetimeObservation = safeObservation &&
            IsQueryDeclarationBindingLoad(query, valueNames, *load) &&
            load->GetDebugLocation().GetBeginPos().line != query.line;
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr && namedLocation) {
            auto range = GetContestRangeForValue(state, load->GetResult());
            std::cerr << "[RangeAnalysisLoadMap] query=" << query.variableName << '@' << query.line
                      << " loadLine=" << load->GetDebugLocation().GetBeginPos().line
                      << " safe=" << safeObservation << " range="
                      << FormatContestRange(range, GetQueryValueType(load->GetResult())) << '\n';
        }
        if (!safeObservation) {
            continue;
        }
        auto type = GetQueryValueType(load->GetResult());
        auto observed = RangeAnalysis::GetBoundedLoopObservedRange(&expr);
        auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, load->GetResult());
        query.hasBoundedPointResult = query.hasBoundedPointResult || observed != nullptr;
        if (lifetimeObservation && query.hasDeclarationStoreResult) {
            continue;
        }
        auto result = FormatContestRange(range, type);
        if (range == nullptr) {
            query.hasUnknownPointObservation = true;
        }
        const bool shouldRecord = ShouldRecordContestResult(query, result, type, range,
            /* fromBeforeProgramPoint = */ false, /* refinesBeforeProgramPoint = */ !lifetimeObservation,
            /* aggregatesDeclaredBindingLifetime = */ lifetimeObservation);
        if (!lifetimeObservation) {
            query.hasDirectPointLoadResult = true;
        }
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
    }
}

struct ReadModifyWriteObservation {
    std::unique_ptr<ValueRange> range;
    bool found{false};
    bool complete{true};
};

struct ReadModifyWriteDependencies {
    std::vector<const Load*> loads;
    bool found{false};
    bool complete{true};
};

void CollectReadModifyWriteDependencies(const RangeDomain& state, Value* value, Value* location,
    std::unordered_set<Value*>& visited, ReadModifyWriteDependencies& dependencies)
{
    constexpr size_t MAX_RMW_DEPENDENCY_VALUES = 32;
    if (value == nullptr || !dependencies.complete || !visited.emplace(value).second) {
        return;
    }
    if (visited.size() > MAX_RMW_DEPENDENCY_VALUES) {
        dependencies.complete = false;
        return;
    }
    auto expression = GetLocalDefiningExpression(value);
    if (expression == nullptr) {
        return;
    }
    if (expression->GetExprKind() == ExprKind::LOAD) {
        auto load = StaticCast<const Load*>(expression);
        if (load->GetLocation() != location) {
            auto targetObject = state.CheckAbstractObjectRefBy(location);
            auto loadObject = state.CheckAbstractObjectRefBy(load->GetLocation());
            if (targetObject == nullptr || loadObject == nullptr) {
                dependencies.complete = false;
                return;
            }
            if (targetObject != loadObject) {
                return;
            }
        }
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            const auto& loadLocation = expression->GetDebugLocation();
            std::cerr << "[RangeAnalysisRmwLoad] line=" << loadLocation.GetBeginPos().line
                      << " columns=" << loadLocation.GetBeginPos().column << '-'
                      << loadLocation.GetEndPos().column << '\n';
        }
        dependencies.found = true;
        dependencies.loads.emplace_back(load);
        return;
    }
    for (auto operand : expression->GetOperands()) {
        CollectReadModifyWriteDependencies(state, operand, location, visited, dependencies);
    }
}

struct ContestMappedValue {
    const Expression* expression;
    Value* value;
};

bool HasSameContestQueryMappingSpan(const DebugLocation& lhs, const DebugLocation& rhs)
{
    return lhs.GetFileName() == rhs.GetFileName() &&
        lhs.GetBeginPos().line == rhs.GetBeginPos().line &&
        lhs.GetBeginPos().column == rhs.GetBeginPos().column &&
        lhs.GetEndPos().line == rhs.GetEndPos().line &&
        lhs.GetEndPos().column == rhs.GetEndPos().column;
}

std::vector<ContestMappedValue> MapReadModifyWriteQueryValues(
    const Store& store, const ReadModifyWriteDependencies& dependencies)
{
    std::vector<ContestMappedValue> mapped;
    mapped.reserve(dependencies.loads.size() + 1);
    bool mapsStoredValue = false;
    for (auto load : dependencies.loads) {
        mapped.emplace_back(ContestMappedValue{load, load->GetResult()});
        mapsStoredValue = mapsStoredValue ||
            HasSameContestQueryMappingSpan(load->GetDebugLocation(), store.GetDebugLocation());
    }
    if (mapsStoredValue) {
        mapped.emplace_back(ContestMappedValue{&store, store.GetValue()});
    }
    return mapped;
}

ReadModifyWriteObservation GetReadModifyWriteObservation(
    const RangeDomain& state, const Store& store)
{
    ReadModifyWriteDependencies dependencies;
    std::unordered_set<Value*> visited;
    CollectReadModifyWriteDependencies(state, store.GetValue(), store.GetLocation(), visited, dependencies);

    ReadModifyWriteObservation observation;
    observation.found = dependencies.found;
    observation.complete = dependencies.complete;
    if (!observation.found || !observation.complete) {
        return observation;
    }
    for (const auto& mapped : MapReadModifyWriteQueryValues(store, dependencies)) {
        auto observed = RangeAnalysis::GetBoundedLoopObservedRange(mapped.expression);
        auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, mapped.value);
        if (range == nullptr) {
            observation.complete = false;
            break;
        }
        if (observation.range == nullptr) {
            observation.range = range->Clone();
        } else if (observation.range->GetRangeKind() != range->GetRangeKind()) {
            observation.complete = false;
        } else if (auto joined = observation.range->Join(*range); joined.has_value()) {
            observation.range = std::move(joined.value());
        }
    }
    return observation;
}

void ResolveQueryAtStoreValue(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const Expression& expr, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    if (expr.GetExprKind() != ExprKind::STORE || state.IsBottom()) {
        return;
    }
    auto store = StaticCast<const Store*>(&expr);
    auto location = store->GetLocation();
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr && IsGlobalVarInCurrentPackage(location)) {
        std::cerr << "[RangeAnalysisGlobalStore] name=" << location->GetSrcCodeIdentifier()
                  << " line=" << expr.GetDebugLocation().GetBeginPos().line << '\n';
    }
    for (auto& query : queries) {
        const bool namedLocation = query.valid && (HasValueNameForQuery(valueNames, location, query) ||
            (IsGlobalVarInCurrentPackage(location) &&
                query.variableName == location->GetSrcCodeIdentifier()));
        if (!namedLocation || !IsSameQueryLocation(query, expr.GetDebugLocation(), contestRoot)) {
            continue;
        }
        auto type = GetQueryValueType(location);
        auto observed = RangeAnalysis::GetBoundedLoopObservedRange(&expr);
        query.hasBoundedLifetimeResult = query.hasBoundedLifetimeResult || observed != nullptr;
        query.hasBoundedPointResult = query.hasBoundedPointResult || observed != nullptr;
        auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, store->GetValue());
        auto readModifyWrite = GetReadModifyWriteObservation(state, *store);
        const bool hadReadModifyWriteResult = query.hasReadModifyWriteResult;
        query.hasReadModifyWriteResult = query.hasReadModifyWriteResult ||
            (readModifyWrite.found && readModifyWrite.complete && readModifyWrite.range != nullptr);
        // A plain assignment executes after the query point and must not
        // overwrite a value already observed before that source line. Keep
        // read-modify-write observations, because they intentionally describe
        // both the value read at the point and the value written back.
        if (query.hasBeforePointResult && !readModifyWrite.found) {
            continue;
        }
        std::unique_ptr<ValueRange> combined;
        if (readModifyWrite.found && readModifyWrite.complete && readModifyWrite.range != nullptr) {
            combined = readModifyWrite.range->Clone();
        } else if (!readModifyWrite.found && readModifyWrite.complete && range != nullptr) {
            combined = range->Clone();
        }
        range = combined.get();
        auto result = FormatContestRange(range, type);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisStoreMap] query=" << query.variableName << '@' << query.line
                      << " storeLine=" << expr.GetDebugLocation().GetBeginPos().line
                      << " rmwFound=" << readModifyWrite.found
                      << " rmwComplete=" << readModifyWrite.complete
                      << " rmw=" << FormatContestRange(readModifyWrite.range.get(), type)
                      << " bounded=" << (observed != nullptr)
                      << " combined=" << result << '\n';
        }
        if (range == nullptr || !readModifyWrite.complete) {
            query.hasUnknownPointObservation = true;
        }
        const bool hasCompleteBoundedPointObservation = observed != nullptr &&
            readModifyWrite.found && readModifyWrite.complete && range != nullptr &&
            !IsContestTopRange(range, type);
        if (hasCompleteBoundedPointObservation && !hadReadModifyWriteResult &&
            (!query.resolved || IsContestTopRange(query.resultRange.get(), type))) {
            SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
            continue;
        }
        if (query.hasUnknownPointObservation || !readModifyWrite.complete ||
            IsContestTopRange(range, type)) {
            SetContestQueryResult(query, type, nullptr, ContestResultOrigin::CHIR_ANALYSIS);
            continue;
        }
        if (!ShouldRecordContestResult(query, result, type, range,
                /* fromBeforeProgramPoint = */ false, /* refinesBeforeProgramPoint = */ false,
                /* aggregatesDeclaredBindingLifetime = */ true)) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS);
        query.hasDeclarationStoreResult = !readModifyWrite.found &&
            IsQueryDeclarationBinding(query, valueNames, location, store->GetDebugLocation());
    }
}

void CollectContextCandidatesFromVisibleNames(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const ValueNameMap& valueNames, const DebugLocation& location,
    const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    auto useScope = location.GetScopeInfo();
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid || !IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        for (auto value : FindVisibleNamedValues(query, valueNames, useScope)) {
            auto type = GetQueryValueType(value);
            if (query.typeHint != ContestQueryTypeHint::UNKNOWN &&
                query.typeHint != GetQueryTypeHint(type)) {
                continue;
            }
            auto observation = ObserveContestRange(state, value);
            if (observation.kind == ContestRangeObservationKind::ABSENT) {
                continue;
            }
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisContextCandidate] kind=visible query="
                          << query.variableName << '@' << query.line
                          << " range=" << FormatContestRange(observation.range, type) << '\n';
            }
            MergeContestContextCandidate(candidates, index, type, observation.range,
                observation.kind == ContestRangeObservationKind::UNKNOWN, /* auxiliary = */ true);
        }
    }
}

void CollectContextCandidateAtDebug(const std::vector<ContestQuery>& queries, ContestContextCandidateMap& candidates,
    ValueNameMap& valueNames, const Debug& debug, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    RememberValueName(valueNames, debug, contestRoot);
    if (!debug.GetValue()->GetType()->IsRef()) {
        auto type = GetQueryValueType(debug.GetValue());
        auto observation = ObserveContestRange(state, debug.GetValue());
        if (observation.kind != ContestRangeObservationKind::ABSENT) {
            for (size_t index = 0; index < queries.size(); ++index) {
                const auto& query = queries[index];
                if (!query.valid || query.variableName != debug.GetSrcCodeIdentifier()) {
                    continue;
                }
                if (!IsSameQueryLocation(query, debug.GetDebugLocation(), contestRoot)) {
                    continue;
                }
                if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                    std::cerr << "[RangeAnalysisContextCandidate] kind=debug query="
                              << query.variableName << '@' << query.line
                              << " range=" << FormatContestRange(observation.range, type) << '\n';
                }
                MergeContestContextCandidate(candidates, index, type, observation.range,
                    observation.kind == ContestRangeObservationKind::UNKNOWN);
            }
        }
    }
    CollectContextCandidatesFromVisibleNames(
        queries, candidates, valueNames, debug.GetDebugLocation(), state, contestRoot);
}

void CollectContextCandidateAtValue(const std::vector<ContestQuery>& queries, ContestContextCandidateMap& candidates,
    const ValueNameMap& valueNames, const DebugLocation& location, Value* value, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid || !HasValueNameForQuery(valueNames, value, query)) {
            continue;
        }
        if (!IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        auto type = GetQueryValueType(value);
        auto observation = ObserveContestRange(state, value);
        if (observation.kind == ContestRangeObservationKind::ABSENT) {
            continue;
        }
        MergeContestContextCandidate(candidates, index, type, observation.range,
            observation.kind == ContestRangeObservationKind::UNKNOWN);
    }
}

void CollectContextCandidateAtExpressionOperands(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const ValueNameMap& valueNames, const Expression& expr,
    const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    if (expr.GetExprKind() == ExprKind::LOAD) {
        auto load = StaticCast<const Load*>(&expr);
        for (size_t index = 0; index < queries.size(); ++index) {
            const auto& query = queries[index];
            if (query.valid && HasValueNameForQuery(valueNames, load->GetLocation(), query) &&
                IsSafeForwardQueryLoad(query, valueNames, *load, contestRoot)) {
                auto type = GetQueryValueType(load->GetResult());
                auto observation = ObserveContestRange(state, load->GetResult());
                if (observation.kind == ContestRangeObservationKind::ABSENT) {
                    continue;
                }
                MergeContestContextCandidate(candidates, index, type, observation.range,
                    observation.kind == ContestRangeObservationKind::UNKNOWN);
            }
        }
    }
    for (auto operand : expr.GetOperands()) {
        if (expr.GetExprKind() == ExprKind::STORE &&
            operand == StaticCast<const Store*>(&expr)->GetLocation()) {
            continue;
        }
        CollectContextCandidateAtValue(queries, candidates, valueNames, expr.GetDebugLocation(), operand, state, contestRoot);
    }
    CollectContextCandidatesFromVisibleNames(
        queries, candidates, valueNames, expr.GetDebugLocation(), state, contestRoot);
}

void ResolveQueryAtExpressionOperands(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const Expression& expr, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    ResolveQueryAtUnnamedLoopInductionLoad(queries, valueNames, expr, state, contestRoot);
    ResolveQueryAtLoadResult(queries, valueNames, expr, state, contestRoot);
    ResolveQueryAtStoreValue(queries, valueNames, expr, state, contestRoot);
    for (auto operand : expr.GetOperands()) {
        if (expr.GetExprKind() == ExprKind::STORE &&
            operand == StaticCast<const Store*>(&expr)->GetLocation()) {
            continue;
        }
        ResolveQueryAtValue(queries, valueNames, expr.GetDebugLocation(), operand, state, contestRoot);
    }
    ResolveQueriesFromVisibleNames(queries, valueNames, expr.GetDebugLocation(), state, contestRoot);
}

// 带 visited 集合检查 CFG 可达性，避免环路递归。
// 识别 RangePropagation 不应折叠掉的循环分支条件表达式。
bool IsLoopBranchConditionExpr(const Expression& expr)
{
    auto parent = expr.GetParentBlock();
    if (parent == nullptr || parent->GetTerminator()->GetExprKind() != ExprKind::BRANCH) {
        return false;
    }
    auto branch = StaticCast<const Branch*>(parent->GetTerminator());
    if (branch->GetCondition() != expr.GetResult()) {
        return false;
    }
    auto trueReachability = CanReachBlockForQueryMapping(branch->GetTrueBlock(), parent);
    if (trueReachability != ContestReachability::UNREACHABLE) {
        return true;
    }
    return CanReachBlockForQueryMapping(branch->GetFalseBlock(), parent) != ContestReachability::UNREACHABLE;
}

// 按查询顺序写入 output.txt，未解析项使用 fallback。
std::string GetContestQueryOutput(ContestQuery& query)
{
    if (query.resolved && !query.result.empty()) {
        return query.result;
    }
    query.resultOrigin = ContestResultOrigin::UNRESOLVED_CHIR_TOP;
    return FormatTopRange(query);
}

const char* ContestResultOriginName(ContestResultOrigin origin)
{
    switch (origin) {
        case ContestResultOrigin::CHIR_ANALYSIS:
            return "CHIRAnalysis";
        case ContestResultOrigin::CONTEXT_SUMMARY:
            return "ContextSummary";
        case ContestResultOrigin::UNRESOLVED_CHIR_TOP:
            return "UnresolvedCHIRTop";
        case ContestResultOrigin::NONE:
            return "None";
    }
    return "None";
}

void ApplyContestContextCandidates(std::vector<ContestQuery>& queries,
    const ContestContextCandidateMap& candidates, bool contextClosureComplete)
{
    for (const auto& [index, candidate] : candidates) {
        if (index >= queries.size() || candidate.type == nullptr) {
            continue;
        }
        auto& query = queries[index];
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisContextApply] query=" << query.variableName << '@' << query.line
                      << " candidate=" << FormatContestRange(candidate.range.get(), candidate.type)
                      << " unknown=" << candidate.hasUnknownObservation
                      << " auxiliaryOnly=" << candidate.auxiliaryOnly
                      << " incomplete=" << candidate.incompleteGlobalLifetime
                      << " initializerSeen=" << candidate.sawInitializerObservation
                      << " exitSeen=" << candidate.sawExitObservation
                      << " global=" << candidate.fromGlobalAccess
                      << " rootUnknown=" << query.hasUnknownPointObservation
                      << " rootRmw=" << query.hasReadModifyWriteResult
                      << " rootTop=" << IsContestTopRange(query.resultRange.get(), query.type)
                      << " root=" << query.result << '\n';
        }
        const bool hasCompleteRootResult = query.resolved &&
            query.resultOrigin != ContestResultOrigin::NONE && query.resultRange != nullptr &&
            !query.hasUnknownPointObservation &&
            !IsContestTopRange(query.resultRange.get(), query.type);
        const bool hasCompleteContextCandidate = !candidate.fromGlobalAccess &&
            !candidate.incompleteGlobalLifetime && !candidate.hasUnknownObservation &&
            !candidate.auxiliaryOnly && candidate.range != nullptr &&
            !query.hasReadModifyWriteResult;
        if (!contextClosureComplete && !candidate.fromGlobalAccess) {
            // A partial call closure is useful for diagnostics but is not an
            // exhaustive set of executions. Keep the root CHIR result, which
            // is a safe over-approximation, instead of narrowing it with a
            // subset of observed contexts.
            continue;
        }
        if (hasCompleteRootResult && !candidate.fromGlobalAccess) {
            auto type = query.type == nullptr ? candidate.type : query.type;
            if (!query.hasBoundedPointResult && hasCompleteContextCandidate &&
                AreContestTypesCompatible(type, candidate.type) &&
                query.resultRange->GetRangeKind() == candidate.range->GetRangeKind() &&
                IsContestRangeSubset(*candidate.range, *query.resultRange)) {
                SetContestQueryResult(
                    query, type, candidate.range.get(), ContestResultOrigin::CONTEXT_SUMMARY);
            }
            continue;
        }
        if (candidate.hasUnknownObservation) {
            query.hasUnknownPointObservation = true;
        }
        auto type = query.type == nullptr ? candidate.type : query.type;
        auto joined = candidate.incompleteGlobalLifetime || candidate.hasUnknownObservation
            ? MakeContestTopRange(type)
            : CloneContestRangeOrTop(candidate.type, candidate.range.get());
        if (joined == nullptr) {
            continue;
        }
        if (!AreContestTypesCompatible(type, candidate.type)) {
            joined = MakeContestTopRange(type);
        } else if (hasCompleteRootResult) {
            auto current = query.resultRange->Clone();
            if (current->GetRangeKind() != joined->GetRangeKind()) {
                joined = MakeContestTopRange(type);
            } else if (auto updated = current->Join(*joined); updated.has_value()) {
                joined = std::move(updated.value());
            } else {
                continue;
            }
        }
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisContextDecision] query=" << query.variableName << '@' << query.line
                      << " compatible=" << AreContestTypesCompatible(type, candidate.type)
                      << " rootType=" << static_cast<int>(GetQueryTypeHint(type))
                      << " candidateType=" << static_cast<int>(GetQueryTypeHint(candidate.type))
                      << " selected=" << FormatContestRange(joined.get(), type) << '\n';
        }
        SetContestQueryResult(
            query, type, joined.get(), ContestResultOrigin::CONTEXT_SUMMARY);
    }
}

void WriteContestOutput(std::vector<ContestQuery>& queries, const ContestInputContext& inputContext)
{
    auto outputPath = inputContext.rootPath / CONTEST_OUTPUT_FILE;
    std::ofstream output(outputPath.string(), std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    for (size_t index = 0; index < queries.size(); ++index) {
        auto& query = queries[index];
        auto current = GetContestQueryOutput(query);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisOutput] " << query.fileName << ':' << query.line << ':'
                      << query.variableName << " origin=" << ContestResultOriginName(query.resultOrigin)
                      << " value=" << current << " resolved=" << query.resolved
                      << " raw=" << query.result << '\n';
        }
        output << current << '\n';
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
    CHIRBuilder& builder, RangeAnalysisWrapper* rangeAnalysisWrapper, DiagnosticEngine& diag, bool enIncre)
    : builder(builder), analysisWrapper(rangeAnalysisWrapper), diag(diag), enIncre(enIncre)
{
}

const OptEffectCHIRMap& RangePropagation::GetEffectMap() const
{
    return effectMap;
}

const std::vector<const Function*>& RangePropagation::GetFuncsNeedRemoveBlocks() const
{
    return funcsNeedRemoveBlocks;
}

void RangePropagation::RunOnPackage(const Ptr<const Package>& package, bool isDebug)
{
    for (auto func : package->GetGlobalFuncsWithBody()) {
        RunOnFunc(func, isDebug);
    }
}

// 重写可证明的常量和分支目标，同时保护循环分支条件。
void RangePropagation::RunOnFunc(const Ptr<const Function>& func, bool isDebug)
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
        if (IsLoopBranchConditionExpr(*expr)) {
            return;
        }
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

// 遍历缓存的 RangeAnalysis 状态并生成竞赛查询输出。
void RangePropagation::EmitContestOutput(const Ptr<const Package>& package, RangeAnalysisWrapper& rangeAnalysisWrapper,
    const std::vector<std::string>& contestRootHints, DiagnosticEngine& diag)
{
    auto inputContext = FindContestInputContext(contestRootHints);
    if (!inputContext.has_value()) {
        return;
    }
    auto queries = LoadContestQueries(inputContext.value());
    if (!queries.has_value()) {
        return;
    }
    auto contestRoot = inputContext.value().rootPath;
    BindGlobalQueries(package, queries.value(), contestRoot);
    BindUnambiguousGlobalQueries(package, queries.value(), contestRoot);
    ValueNameMap valueNames;
    ContestAggregateMap aggregates;
    ContestContextCandidateMap contextCandidates;
    ContestContextLoopObservationMap contextLoopObservations;
    SeedBoundGlobalInitializers(queries.value(), contextCandidates);
    ValueNameMap contextValueNames;
    const auto actionBeforeVisitExpr = [&queries, &valueNames, &contextCandidates, &contestRoot](
                                           const RangeDomain& state, Expression* expression, size_t) {
        CollectContextCandidateAtGlobalAccess(
            queries.value(), contextCandidates, *expression, state, contestRoot,
            /* boundDeclarationsOnly = */ true);
        ResolveQueryBeforeGlobalAccess(queries.value(), *expression, state, contestRoot);
        ResolveQueriesFromVisibleNames(queries.value(), valueNames,
            expression->GetDebugLocation(), state, contestRoot, /* beforeProgramPoint = */ true);
    };
    auto actionAfterVisitExpr = [&queries, &valueNames, &aggregates, &contestRoot](
                                    const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
            ResolveQueryAtDebug(queries.value(), valueNames, aggregates, *StaticCast<Debug*>(expr), state, contestRoot);
            return;
        }
        ResolveQueryAtExpressionOperands(queries.value(), valueNames, *expr, state, contestRoot);
    };
    const auto actionOnTerminator = [&queries, &valueNames, &contestRoot](
                                        const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
        if (terminator == nullptr) {
            return;
        }
        ResolveQueryAtExpressionOperands(queries.value(), valueNames, *terminator, state, contestRoot);
    };
    auto resolveQueries = [&](Results<RangeDomain>& result) {
        result.VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
    };
    const auto contextActionBeforeVisitExpr = [&queries, &contextCandidates, &contestRoot](
                                                  const RangeDomain& state, Expression* expression, size_t) {
        CollectContextCandidateAtGlobalAccess(
            queries.value(), contextCandidates, *expression, state, contestRoot);
    };
    const auto contextActionAfterVisitExpr = [&queries, &contextCandidates, &contextValueNames, &contestRoot](
                                                const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
            CollectContextCandidateAtDebug(
                queries.value(), contextCandidates, contextValueNames, *StaticCast<Debug*>(expr), state, contestRoot);
            return;
        }
        CollectContextCandidateAtExpressionOperands(
            queries.value(), contextCandidates, contextValueNames, *expr, state, contestRoot);
    };
    const auto collectContextTerminator =
        [&queries, &contextCandidates, &contextValueNames, &contestRoot](
            const RangeDomain& state, Terminator* terminator, bool collectLifetimeExit) {
        if (terminator == nullptr) {
            return;
        }
        if (collectLifetimeExit) {
            CollectContextCandidateAtGlobalExit(
                queries.value(), contextCandidates, *terminator, state);
        }
        CollectContextCandidateAtExpressionOperands(
            queries.value(), contextCandidates, contextValueNames, *terminator, state, contestRoot);
    };
    const auto contextActionOnTerminator =
        [&collectContextTerminator](
            const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
            collectContextTerminator(state, terminator, /* collectLifetimeExit = */ false);
        };
    bool contextClosureComplete = true;
    auto collectContextCandidates = [&](const Function* rootFunction, Results<RangeDomain>& root) {
        const bool collectLifetimeExit = rootFunction != nullptr &&
            rootFunction->GetFuncKind() == FuncKind::MAIN_ENTRY;
        const auto contextRootActionOnTerminator =
            [&collectContextTerminator, collectLifetimeExit](
                const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
                collectContextTerminator(state, terminator, collectLifetimeExit);
            };
        root.VisitWith(
            contextActionBeforeVisitExpr, contextActionAfterVisitExpr,
            contextRootActionOnTerminator);
        contextClosureComplete = RangeAnalysis::VisitReachableContextSensitiveResults(rootFunction, root,
            [&](const Function*, const std::string&, Results<RangeDomain>& result) {
                result.VisitWith(
                    contextActionBeforeVisitExpr, contextActionAfterVisitExpr, contextActionOnTerminator);
                contextClosureComplete =
                    CollectContestContextLoopObservations(
                        contextLoopObservations, result) &&
                    contextClosureComplete;
            }) && contextClosureComplete;
    };
    auto relevantFunctions = CollectContestRelevantFunctions(package, queries.value(), contestRoot);
    auto reverseCallGraph = BuildReverseContestCallGraph(package);
    const bool hasMainContextRoot = std::any_of(
        relevantFunctions.begin(), relevantFunctions.end(), [](const auto* function) {
            return function != nullptr && function->GetFuncKind() == FuncKind::MAIN_ENTRY;
        });
    auto packageInit = package->GetPackageInitFunc();

    RangeAnalysis::ClearContextSensitiveResults();
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (!RangeAnalysis::Filter(*func) || relevantFunctions.find(func) == relevantFunctions.end()) {
            continue;
        }
        auto callers = reverseCallGraph.find(func);
        const bool hasDirectCaller =
            callers != reverseCallGraph.end() && !callers->second.empty();
        const bool isContextRoot = func->GetFuncKind() == FuncKind::MAIN_ENTRY ||
            func == packageInit || (!hasMainContextRoot && !hasDirectCaller);
        auto result = rangeAnalysisWrapper.RunOnFunc(func, /* isDebug = */ false, diag);
        if (result != nullptr) {
            resolveQueries(*result);
            if (isContextRoot) {
                collectContextCandidates(func, *result);
            }
        } else if (auto cachedResult = rangeAnalysisWrapper.CheckFuncResult(func); cachedResult != nullptr) {
            resolveQueries(*cachedResult);
            if (isContextRoot) {
                collectContextCandidates(func, *cachedResult);
            }
        }
    }

    FinalizeBoundGlobalCandidateCompleteness(
        queries.value(), contextCandidates, hasMainContextRoot);
    ApplyContestContextCandidates(
        queries.value(), contextCandidates, contextClosureComplete);
    ApplyContestAggregates(
        queries.value(), aggregates, contextLoopObservations);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisContextClosure] complete="
                  << contextClosureComplete << '\n';
    }
    WriteContestOutput(queries.value(), inputContext.value());
    RangeAnalysis::ClearContextSensitiveResults();
    RangeAnalysis::ClearBoundedLoopObservedRanges();
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
    if (IsGlobalVarInCurrentPackage(loc)) {
        // let a = 3
        // Load(gv_a)
        gv = DynamicCast<GlobalVar*>(loc);
    } else if (loc->IsLocalVar()) {
        // let sa = SA(); sa.x
        // %0 = GetElementRef(gv_sa); %1 = Load(%0)
        auto locExpr = StaticCast<LocalVar*>(loc)->GetExpr();
        if (locExpr->GetExprKind() == ExprKind::GET_ELEMENT_REF) {
            auto base = StaticCast<GetElementRef*>(locExpr)->GetLocation();
            if (IsGlobalVarInCurrentPackage(base)) {
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
            if (IsGlobalVarInCurrentPackage(loc)) {
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
void RangePropagation::RecordEffectMap(const Expression* expr, const Function* func) const
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
        effectMap[gv].emplace(const_cast<Function*>(func));
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
                diag.DiagnoseRefactor(DiagKindRefactor::chir_idx_out_of_bounds, ToRange(intrin->GetDebugLocation()));
            std::stringstream ss;
            ss << "range of index " << i - begin << " is (" << indexRange.ToString()
               << "), however the size of varray is " + std::to_string(size);
            bd.AddMainHintArguments(ss.str());
        }
    }
}
} // namespace Cangjie::CHIR
