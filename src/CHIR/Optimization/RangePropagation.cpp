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
#include <chrono>
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

struct RangePropagationPerfStats {
    uint64_t reverseCallGraphCalls{0};
    uint64_t reverseCallGraphNanos{0};
    uint64_t relevantFunctionCalls{0};
    uint64_t relevantFunctionNanos{0};
    uint64_t runFunctionCalls{0};
    uint64_t runFunctionNanos{0};
    uint64_t emitNanos{0};
    size_t maxPackageFunctions{0};
    size_t maxRelevantFunctions{0};
    size_t maxContextRoots{0};
};

RangePropagationPerfStats rangePropagationPerfStats;

bool IsRangePropagationPerfTraceEnabled()
{
    static const bool enabled = std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr;
    return enabled;
}

class ScopedRangePropagationPerfTimer {
public:
    explicit ScopedRangePropagationPerfTimer(uint64_t* destination)
        : destination(IsRangePropagationPerfTraceEnabled() ? destination : nullptr),
          start(this->destination == nullptr ? Clock::time_point{} : Clock::now())
    {
    }

    ~ScopedRangePropagationPerfTimer()
    {
        if (destination == nullptr) {
            return;
        }
        *destination += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }

private:
    using Clock = std::chrono::steady_clock;
    uint64_t* destination;
    Clock::time_point start;
};

double RangePropagationPerfMilliseconds(uint64_t nanoseconds)
{
    return static_cast<double>(nanoseconds) / 1000000.0;
}

void PrintAndResetRangePropagationPerfStats()
{
    if (IsRangePropagationPerfTraceEnabled()) {
        std::cerr << "[RangePropagationPerf]"
                  << " reverse-call-graph-calls=" << rangePropagationPerfStats.reverseCallGraphCalls
                  << " reverse-call-graph-ms="
                  << RangePropagationPerfMilliseconds(rangePropagationPerfStats.reverseCallGraphNanos)
                  << " relevant-function-calls=" << rangePropagationPerfStats.relevantFunctionCalls
                  << " relevant-function-ms="
                  << RangePropagationPerfMilliseconds(rangePropagationPerfStats.relevantFunctionNanos)
                  << " run-function-calls=" << rangePropagationPerfStats.runFunctionCalls
                  << " run-function-ms="
                  << RangePropagationPerfMilliseconds(rangePropagationPerfStats.runFunctionNanos)
                  << " emit-ms="
                  << RangePropagationPerfMilliseconds(rangePropagationPerfStats.emitNanos)
                  << " max-package-functions=" << rangePropagationPerfStats.maxPackageFunctions
                  << " max-relevant-functions=" << rangePropagationPerfStats.maxRelevantFunctions
                  << " max-context-roots=" << rangePropagationPerfStats.maxContextRoots
                  << '\n';
    }
    rangePropagationPerfStats = RangePropagationPerfStats{};
}

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

enum class ContestProgramPointKind : uint8_t {
    BEFORE_EXPRESSION,
    AFTER_EXPRESSION,
    DIRECT_LOAD_RESULT,
    BOUNDED_LOOP_POINT,
    BOUNDED_LOOP_LIFETIME,
    DECLARATION_STORE,
    DECLARATION_LIFETIME,
    READ_MODIFY_WRITE,
    BOUND_GLOBAL_PRESTATE,
    GLOBAL_PRESTATE,
    LOOP_LIFETIME_SUMMARY,
    CONTEXT_SUMMARY,
};

struct ContestProgramPoint {
    static constexpr size_t INVALID_EXPRESSION_INDEX = std::numeric_limits<size_t>::max();

    const Block* block{nullptr};
    const Expression* expression{nullptr};
    size_t expressionIndex{INVALID_EXPRESSION_INDEX};
    ContestProgramPointKind kind{ContestProgramPointKind::AFTER_EXPRESSION};

    bool IsBeforeExpression() const
    {
        return kind == ContestProgramPointKind::BEFORE_EXPRESSION ||
            kind == ContestProgramPointKind::BOUND_GLOBAL_PRESTATE ||
            kind == ContestProgramPointKind::GLOBAL_PRESTATE;
    }

    bool RefinesBeforeExpression() const
    {
        return kind == ContestProgramPointKind::DIRECT_LOAD_RESULT ||
            kind == ContestProgramPointKind::BOUNDED_LOOP_POINT;
    }

    bool AggregatesDeclaredBindingLifetime() const
    {
        return kind == ContestProgramPointKind::BOUNDED_LOOP_LIFETIME ||
            kind == ContestProgramPointKind::DECLARATION_STORE ||
            kind == ContestProgramPointKind::DECLARATION_LIFETIME ||
            kind == ContestProgramPointKind::READ_MODIFY_WRITE ||
            kind == ContestProgramPointKind::LOOP_LIFETIME_SUMMARY;
    }
};

ContestProgramPoint MakeContestProgramPoint(
    const Expression* expression, ContestProgramPointKind kind)
{
    ContestProgramPoint point;
    point.expression = expression;
    point.kind = kind;
    if (expression == nullptr) {
        return point;
    }
    point.block = expression->GetParentBlock();
    if (point.block == nullptr) {
        return point;
    }
    size_t index = 0;
    for (auto candidate : point.block->GetExpressions()) {
        if (candidate == expression) {
            point.expressionIndex = index;
            return point;
        }
        ++index;
    }
    if (point.block->GetTerminator() == expression) {
        point.expressionIndex = index;
    }
    return point;
}

struct ContestProgramPointState {
    uint32_t observedKinds{0};
    bool hasUnknownObservation{false};
    std::optional<ContestProgramPoint> selectedPoint;

    void Observe(const ContestProgramPoint& point)
    {
        observedKinds |= 1U << static_cast<uint8_t>(point.kind);
    }

    bool Has(ContestProgramPointKind kind) const
    {
        return (observedKinds & (1U << static_cast<uint8_t>(kind))) != 0;
    }

    bool HasBeforeResult() const
    {
        return Has(ContestProgramPointKind::BEFORE_EXPRESSION) ||
            Has(ContestProgramPointKind::BOUND_GLOBAL_PRESTATE) ||
            Has(ContestProgramPointKind::GLOBAL_PRESTATE);
    }

    void Select(const ContestProgramPoint& point)
    {
        Observe(point);
        selectedPoint = point;
    }
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
    bool isUnambiguousGlobalBinding{false};
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    ContestResultOrigin resultOrigin{ContestResultOrigin::NONE};
    bool valid{true};
    bool resolved{false};
    ContestProgramPointState programPoints;
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
    bool sawProgramEntryObservation{false};
    bool sawExitObservation{false};
    bool incompleteGlobalLifetime{false};
    bool hasUnknownObservation{false};
    bool auxiliaryOnly{false};
    bool hasDirectPointLoadObservation{false};
    bool exactSIntObservationUnionComplete{true};
    std::vector<SInt> exactSIntObservations;
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

std::optional<std::string> FormatContestStridedInterval(
    const ConstantRange& range, const Type& type, const SIntCongruence& congruence)
{
    if (!congruence.IsUseful() || range.IsEmptySet()) {
        return std::nullopt;
    }
    if (!range.IsFullSet() &&
        (type.IsUnsignedInteger() ? range.IsWrappedSet() : range.IsSignWrappedSet())) {
        return std::nullopt;
    }
    auto stride = static_cast<__int128>(congruence.stride);
    auto residue = static_cast<__int128>(congruence.residue);
    auto lower = type.IsUnsignedInteger()
        ? static_cast<__int128>(range.IsFullSet() ? SInt::UMinValue(ToWidth(type)).UVal()
                                                  : range.UMinValue().UVal())
        : static_cast<__int128>(range.IsFullSet() ? SInt::SMinValue(ToWidth(type)).SVal()
                                                  : range.SMinValue().SVal());
    auto upper = type.IsUnsignedInteger()
        ? static_cast<__int128>(range.IsFullSet() ? SInt::UMaxValue(ToWidth(type)).UVal()
                                                  : range.UMaxValue().UVal())
        : static_cast<__int128>(range.IsFullSet() ? SInt::SMaxValue(ToWidth(type)).SVal()
                                                  : range.SMaxValue().SVal());
    auto lowerResidue = lower % stride;
    if (lowerResidue < 0) {
        lowerResidue += stride;
    }
    auto first = lower + (residue - lowerResidue + stride) % stride;
    auto upperResidue = upper % stride;
    if (upperResidue < 0) {
        upperResidue += stride;
    }
    auto last = upper - (upperResidue - residue + stride) % stride;
    if (first > last) {
        return std::nullopt;
    }
    if (first == last) {
        return type.IsUnsignedInteger()
            ? FormatUnsignedInt(static_cast<uint64_t>(first))
            : FormatSignedInt(static_cast<int64_t>(first));
    }
    std::stringstream ss;
    ss << "[";
    if (type.IsUnsignedInteger()) {
        ss << static_cast<uint64_t>(first) << ", " << static_cast<uint64_t>(last);
    } else {
        ss << static_cast<int64_t>(first) << ", " << static_cast<int64_t>(last);
    }
    ss << ":" << congruence.stride << "]";
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

std::optional<std::string> FormatContestIntervalsWithExclusions(const ConstantRange& range,
    const Type& type, const std::optional<std::vector<SInt>>& excludedValues)
{
    if (!excludedValues.has_value() || excludedValues->empty() || range.IsEmptySet() ||
        (!range.IsFullSet() && !IsContestPrintableIntegerInterval(range, type))) {
        return std::nullopt;
    }
    const auto lower = type.IsUnsignedInteger()
        ? static_cast<__int128>(range.IsFullSet() ? SInt::UMinValue(ToWidth(type)).UVal()
                                                  : range.UMinValue().UVal())
        : static_cast<__int128>(range.IsFullSet() ? SInt::SMinValue(ToWidth(type)).SVal()
                                                  : range.SMinValue().SVal());
    const auto upper = type.IsUnsignedInteger()
        ? static_cast<__int128>(range.IsFullSet() ? SInt::UMaxValue(ToWidth(type)).UVal()
                                                  : range.UMaxValue().UVal())
        : static_cast<__int128>(range.IsFullSet() ? SInt::SMaxValue(ToWidth(type)).SVal()
                                                  : range.SMaxValue().SVal());
    std::vector<__int128> holes;
    holes.reserve(excludedValues->size());
    for (const auto& value : *excludedValues) {
        auto mathematical = type.IsUnsignedInteger()
            ? static_cast<__int128>(value.UVal())
            : static_cast<__int128>(value.SVal());
        if (mathematical >= lower && mathematical <= upper) {
            holes.emplace_back(mathematical);
        }
    }
    std::sort(holes.begin(), holes.end());
    holes.erase(std::unique(holes.begin(), holes.end()), holes.end());
    if (holes.empty()) {
        return std::nullopt;
    }
    auto formatValue = [&type](__int128 value) {
        return type.IsUnsignedInteger()
            ? FormatUnsignedInt(static_cast<uint64_t>(value))
            : FormatSignedInt(static_cast<int64_t>(value));
    };
    std::vector<std::string> pieces;
    auto appendPiece = [&](const __int128 pieceLower, const __int128 pieceUpper) {
        if (pieceLower > pieceUpper) {
            return;
        }
        if (pieceLower == pieceUpper) {
            pieces.emplace_back(formatValue(pieceLower));
            return;
        }
        pieces.emplace_back("[" + formatValue(pieceLower) + ", " + formatValue(pieceUpper) + ":1]");
    };
    auto cursor = lower;
    for (const auto hole : holes) {
        appendPiece(cursor, hole - 1);
        cursor = hole + 1;
    }
    appendPiece(cursor, upper);
    if (pieces.empty()) {
        return std::nullopt;
    }
    std::stringstream ss;
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << pieces[i];
    }
    return ss.str();
}

std::optional<std::string> FormatContestIntervalFragments(
    const std::optional<std::vector<SIntIntervalFragment>>& fragments, const Type& type)
{
    if (!fragments.has_value() || fragments->empty()) {
        return std::nullopt;
    }
    std::stringstream ss;
    for (size_t i = 0; i < fragments->size(); ++i) {
        if (i != 0) {
            ss << ", ";
        }
        const auto& fragment = (*fragments)[i];
        if (fragment.lower == fragment.upper) {
            ss << FormatSIntValue(fragment.lower, type);
            continue;
        }
        ss << "[" << FormatSIntValue(fragment.lower, type) << ", "
           << FormatSIntValue(fragment.upper, type) << ":"
           << fragment.stride << "]";
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
        // A fragment union carries a separate congruence for each component;
        // the range-wide congruence is only their weaker common divisor.
        if (auto fragments = FormatContestIntervalFragments(
                sintRange.GetIntervalFragments(), *type); fragments.has_value()) {
            return *fragments;
        }
        if (sintRange.GetCongruence().has_value()) {
            auto strided = FormatContestStridedInterval(
                numeric, *type, *sintRange.GetCongruence());
            if (strided.has_value()) {
                return *strided;
            }
        }
        if (auto disjoint = FormatContestIntervalsWithExclusions(
                numeric, *type, sintRange.GetExcludedValues()); disjoint.has_value()) {
            return *disjoint;
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
        lhsInt.GetExactValues() == rhsInt.GetExactValues() &&
        lhsInt.GetCongruence() == rhsInt.GetCongruence() &&
        lhsInt.GetKnownBits() == rhsInt.GetKnownBits() &&
        lhsInt.GetExcludedValues() == rhsInt.GetExcludedValues() &&
        lhsInt.GetIntervalFragments() == rhsInt.GetIntervalFragments();
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
    ContestQuery& query, Type* type, const ValueRange* range, ContestResultOrigin origin,
    const ContestProgramPoint* programPoint = nullptr)
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
    if (programPoint != nullptr) {
        query.programPoints.Select(*programPoint);
    }
}

bool ShouldRecordContestResult(
    ContestQuery& query, const std::string& result, Type* type, const ValueRange* range,
    const ContestProgramPoint& programPoint)
{
    const bool hadDirectLoadResult =
        query.programPoints.Has(ContestProgramPointKind::DIRECT_LOAD_RESULT);
    const bool hadBeforeResult = query.programPoints.HasBeforeResult();
    query.programPoints.Observe(programPoint);
    const bool fromBeforeProgramPoint = programPoint.IsBeforeExpression();
    const bool refinesBeforeProgramPoint = programPoint.RefinesBeforeExpression();
    const bool aggregatesDeclaredBindingLifetime =
        programPoint.AggregatesDeclaredBindingLifetime();
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisRecord] query=" << query.variableName << '@' << query.line
                  << " candidate=" << result
                  << " current=" << (query.resolved ? query.result : "<unresolved>")
                  << " before=" << fromBeforeProgramPoint
                  << " refine=" << refinesBeforeProgramPoint
                  << " lifetime=" << aggregatesDeclaredBindingLifetime
                  << " block=" << programPoint.block
                  << " exprIndex=" << programPoint.expressionIndex
                  << " boundedPoint=" << query.programPoints.Has(
                         ContestProgramPointKind::BOUNDED_LOOP_POINT)
                  << " boundedLifetime=" << query.programPoints.Has(
                         ContestProgramPointKind::BOUNDED_LOOP_LIFETIME)
                  << " directLoad=" << query.programPoints.Has(
                         ContestProgramPointKind::DIRECT_LOAD_RESULT)
                  << '\n';
    }
    if (!aggregatesDeclaredBindingLifetime) {
        if (fromBeforeProgramPoint && hadDirectLoadResult) {
            return false;
        }
        if (hadBeforeResult && !fromBeforeProgramPoint) {
            if (!refinesBeforeProgramPoint) {
                return false;
            }
            if (!hadDirectLoadResult) {
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
        SetContestQueryResult(query, currentType, nullptr, query.resultOrigin, &programPoint);
        return false;
    }
    if (auto updated = current->Join(*candidate); updated.has_value()) {
        current = std::move(updated.value());
        query.result = FormatContestRange(current.get(), currentType);
        query.resultRange = std::move(current);
        query.programPoints.Select(programPoint);
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
    std::unique_ptr<ValueRange> prefixRange;
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
    std::unique_ptr<ValueRange> prefixEvidence;
    size_t loopUses = 0;

    // Establish that this binding is loop-carried before collecting its full
    // observable lifetime.  A declaration query denotes every reachable value
    // of the binding, including reads and writes after a loop exit.
    for (auto user : binding->GetUsers()) {
        if (user == nullptr || user == aggregateBinding.initializationStore ||
            (user->GetExprKind() == ExprKind::DEBUGEXPR &&
                StaticCast<const Debug*>(user)->GetValue() == binding)) {
            continue;
        }
        const bool directLoad = user->GetExprKind() == ExprKind::LOAD &&
            StaticCast<const Load*>(user)->GetLocation() == binding;
        const bool directStore = user->GetExprKind() == ExprKind::STORE &&
            StaticCast<const Store*>(user)->GetLocation() == binding;
        if (!directLoad && !directStore) {
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
        }
    }
    if (!hasLoopStore) {
        return ContestLoopLifetime{};
    }

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
        if (cycle == ContestReachability::UNKNOWN) {
            lifetime.complete = false;
        }
        if (++loopUses > MAX_CONTEST_LOOP_USES) {
            lifetime.complete = false;
            continue;
        }
        std::unique_ptr<ValueRange> observed;
        std::unique_ptr<ValueRange> prefix;
        auto contextObservation = contextObservations.find(user);
        if (contextObservation != contextObservations.end()) {
            lifetime.hasContextObservation = true;
            if (!contextObservation->second.complete ||
                contextObservation->second.range == nullptr) {
                lifetime.complete = false;
                continue;
            }
            observed = contextObservation->second.range->Clone();
            if (contextObservation->second.prefixRange != nullptr) {
                prefix = contextObservation->second.prefixRange->Clone();
            }
        } else {
            observed = RangeAnalysis::GetBoundedLoopObservedRange(user);
            prefix = RangeAnalysis::GetBoundedLoopPrefixObservedRange(user);
        }
        if (observed == nullptr) {
            lifetime.complete = false;
            continue;
        }
        MergeContestLoopLifetime(lifetime, type, observed.get());
        if (cycle != ContestReachability::UNREACHABLE && prefix != nullptr &&
            observed->GetRangeKind() == prefix->GetRangeKind()) {
            if (prefixEvidence == nullptr) {
                prefixEvidence = std::move(prefix);
            } else if (auto joined = prefixEvidence->Join(*prefix);
                joined.has_value()) {
                prefixEvidence = std::move(joined.value());
            }
        }
    }
    if (lifetime.hasLoopUse && escaped) {
        lifetime.complete = false;
    }
    if (lifetime.complete && lifetime.range != nullptr &&
        prefixEvidence != nullptr &&
        lifetime.range->GetRangeKind() == prefixEvidence->GetRangeKind()) {
        lifetime.range = RangeAnalysis::JoinSupplementalLoopEvidence(
            *lifetime.range, *prefixEvidence);
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
    bool withinBudget = true;
    const auto collectObservation = [&](const RangeDomain& state,
                                        Expression* expression, Value* value) {
        if (!withinBudget || expression == nullptr || value == nullptr ||
            state.IsBottom()) {
            return;
        }
        auto found = observations.find(expression);
        if (found == observations.end()) {
            if (observations.size() >= MAX_CONTEXT_LOOP_OBSERVATIONS) {
                withinBudget = false;
                return;
            }
            found = observations.emplace(
                expression, ContestContextLoopObservation{}).first;
        }

        auto type = GetQueryValueType(value);
        auto bounded = analysis->GetLocalBoundedLoopObservedRange(expression);
        auto sharedBounded = RangeAnalysis::GetBoundedLoopObservedRange(expression);
        auto prefix = analysis->GetLocalBoundedLoopPrefixObservedRange(expression);
        if (prefix == nullptr) {
            prefix = RangeAnalysis::GetBoundedLoopPrefixObservedRange(expression);
        }
        auto direct = ObserveContestRange(state, value);
        std::unique_ptr<ValueRange> selectedWithPrefix;
        const ValueRange* selected = bounded.get();
        if ((selected == nullptr || IsContestTopRange(selected, type)) &&
            sharedBounded != nullptr &&
            !IsContestTopRange(sharedBounded.get(), type)) {
            selected = sharedBounded.get();
        }
        if ((selected == nullptr || IsContestTopRange(selected, type)) &&
            direct.kind == ContestRangeObservationKind::KNOWN) {
            selected = direct.range;
        }
        if (selected != nullptr && prefix != nullptr &&
            selected->GetRangeKind() == prefix->GetRangeKind()) {
            selectedWithPrefix = RangeAnalysis::JoinSupplementalLoopEvidence(
                *selected, *prefix);
            selected = selectedWithPrefix.get();
        }
        if (selected == nullptr &&
            direct.kind == ContestRangeObservationKind::UNKNOWN) {
            auto top = MakeContestTopRange(type);
            if (top == nullptr) {
                found->second.complete = false;
                return;
            }
            if (found->second.range == nullptr) {
                found->second.range = std::move(top);
            } else if (auto joined = found->second.range->Join(*top);
                joined.has_value()) {
                found->second.range = std::move(joined.value());
            }
            return;
        }
        if (selected == nullptr) {
            found->second.complete = false;
            return;
        }
        if (found->second.range == nullptr) {
            found->second.range = selected->Clone();
        } else if (found->second.range->GetRangeKind() != selected->GetRangeKind()) {
            found->second.complete = false;
        } else if (auto joined = found->second.range->Join(*selected);
            joined.has_value()) {
            found->second.range = std::move(joined.value());
        }
        if (prefix != nullptr) {
            if (found->second.prefixRange == nullptr) {
                found->second.prefixRange = std::move(prefix);
            } else if (found->second.prefixRange->GetRangeKind() !=
                prefix->GetRangeKind()) {
                found->second.complete = false;
            } else if (auto joined = found->second.prefixRange->Join(*prefix);
                joined.has_value()) {
                found->second.prefixRange = std::move(joined.value());
            }
        }
    };
    result.VisitWith(
        [&](const RangeDomain& state, Expression* expression, size_t) {
            if (expression != nullptr &&
                expression->GetExprKind() == ExprKind::STORE) {
                collectObservation(state, expression,
                    StaticCast<Store*>(expression)->GetValue());
            }
        },
        [&](const RangeDomain& state, Expression* expression, size_t) {
            if (expression != nullptr &&
                expression->GetExprKind() == ExprKind::LOAD) {
                collectObservation(state, expression,
                    StaticCast<Load*>(expression)->GetResult());
            }
        },
        [](const RangeDomain&, Terminator*, std::optional<Block*>) {});
    return withinBudget;
}

void ApplyContestAggregates(
    std::vector<ContestQuery>& queries, const ContestAggregateMap& aggregates,
    const ContestContextLoopObservationMap& contextObservations,
    bool contextClosureComplete)
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
        const bool hasCompleteContextLifetime =
            contextClosureComplete && combined.hasContextObservation;
        if ((query.programPoints.hasUnknownObservation && !hasCompleteContextLifetime) ||
            combined.range == nullptr) {
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
            auto point = MakeContestProgramPoint(
                nullptr, ContestProgramPointKind::LOOP_LIFETIME_SUMMARY);
            SetContestQueryResult(
                query, type, joined.get(), query.resultOrigin, &point);
            continue;
        }
        // A context summary is precise for a declaration lifetime only when
        // every loop use was observed. Otherwise aggregateRange is null and
        // SetContestQueryResult below deliberately yields a sound Top instead
        // of retaining a point-only summary such as the post-loop load.
        auto point = MakeContestProgramPoint(
            nullptr, ContestProgramPointKind::LOOP_LIFETIME_SUMMARY);
        SetContestQueryResult(
            query, type, aggregateRange, ContestResultOrigin::CHIR_ANALYSIS, &point);
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

bool MayMatchAnyContestQuery(
    const std::vector<ContestQuery>& queries, const DebugLocation& location, const std::filesystem::path& contestRoot)
{
    return std::any_of(queries.begin(), queries.end(), [&](const auto& query) {
        return query.valid && IsSameQueryLocation(query, location, contestRoot);
    });
}

GlobalVar* GetDirectGlobalLocation(const Expression& expression);

bool IsWithinGlobalDeclarationLocation(const ContestQuery& query, const GlobalVar& global,
    const std::filesystem::path& contestRoot)
{
    const auto& location = global.GetDebugLocation();
    if (!IsSameQueryFile(query, location, contestRoot)) {
        return false;
    }
    const auto beginLine = location.GetBeginPos().line;
    const auto endLine = location.GetEndPos().line;
    if (beginLine == 0) {
        return false;
    }
    return beginLine <= query.line &&
        query.line <= std::max(beginLine, endLine);
}

bool IsGlobalInitializerStore(const Expression& expression, const GlobalVar& global)
{
    return expression.GetExprKind() == ExprKind::STORE &&
        GetDirectGlobalLocation(expression) == &global &&
        expression.GetTopLevelFunc() == global.GetInitFunc();
}

void BindGlobalQueries(const Ptr<const Package>& package, std::vector<ContestQuery>& queries,
    const std::filesystem::path& contestRoot)
{
    for (auto& query : queries) {
        GlobalVar* matchedGlobal = nullptr;
        bool matchedDeclaration = false;
        bool ambiguous = false;
        for (auto global : package->GetGlobalVars()) {
            if (!IsGlobalVarInCurrentPackage(global) || query.variableName != global->GetSrcCodeIdentifier() ||
                !IsWithinGlobalDeclarationLocation(query, *global, contestRoot)) {
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
                        matchedDeclaration =
                            matchedDeclaration || IsGlobalInitializerStore(*expression, *global);
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
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisGlobalBind] mode=exact query="
                          << query.variableName << '@' << query.line
                          << " result=" << (ambiguous ? "ambiguous" : "unresolved") << '\n';
            }
            continue;
        }
        query.boundGlobal = matchedGlobal;
        query.isGlobalDeclarationQuery = matchedDeclaration;
        query.type = GetQueryValueType(matchedGlobal);
        query.typeHint = GetQueryTypeHint(query.type);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            const auto& location = matchedGlobal->GetDebugLocation();
            std::cerr << "[RangeAnalysisGlobalBind] mode=exact query="
                      << query.variableName << '@' << query.line
                      << " global=" << static_cast<const void*>(matchedGlobal)
                      << " declaration=" << query.isGlobalDeclarationQuery
                      << " global-range=" << location.GetBeginPos().line
                      << '-' << location.GetEndPos().line << '\n';
        }
    }
}

bool FunctionMayContainContestQuery(
    const Function* func, const ContestQuery& query, const std::filesystem::path& contestRoot);

bool HasPotentialLocalBindingBeforeQuery(const Ptr<const Package>& package, const ContestQuery& query,
    const std::filesystem::path& contestRoot)
{
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (func == nullptr || func->GetBody() == nullptr ||
            !FunctionMayContainContestQuery(func, query, contestRoot)) {
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
                query.variableName != global->GetSrcCodeIdentifier()) {
                continue;
            }
            if (matchedGlobal != nullptr && matchedGlobal != global) {
                ambiguous = true;
                break;
            }
            matchedGlobal = global;
        }
        if (ambiguous || matchedGlobal == nullptr) {
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisGlobalBind] mode=unambiguous query="
                          << query.variableName << '@' << query.line
                          << " result=" << (ambiguous ? "ambiguous" : "unresolved") << '\n';
            }
            continue;
        }
        query.boundGlobal = matchedGlobal;
        // BindGlobalQueries has already checked every surviving declaration,
        // direct Load and direct Store at this source location. If the input
        // still names a unique package global and no local binding can
        // shadow it, the source occurrence was erased or wrapped while
        // lowering. Its whole-program global lifetime is a sound
        // over-approximation of that otherwise unmappable program point.
        query.isGlobalDeclarationQuery = true;
        query.isUnambiguousGlobalBinding = true;
        query.type = GetQueryValueType(matchedGlobal);
        query.typeHint = GetQueryTypeHint(query.type);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            const auto& location = matchedGlobal->GetDebugLocation();
            std::cerr << "[RangeAnalysisGlobalBind] mode=unambiguous query="
                      << query.variableName << '@' << query.line
                      << " global=" << static_cast<const void*>(matchedGlobal)
                      << " declaration=" << query.isGlobalDeclarationQuery
                      << " global-range=" << location.GetBeginPos().line
                      << '-' << location.GetEndPos().line << '\n';
        }
    }
}

// 判断某个值是否已关联指定源码变量名。
bool FunctionMayContainContestQuery(
    const Function* func, const ContestQuery& query, const std::filesystem::path& contestRoot)
{
    if (func == nullptr || func->GetBody() == nullptr || !query.valid) {
        return false;
    }
    for (auto block : func->GetBody()->GetAllBlocks()) {
        for (auto expr : block->GetExpressions()) {
            if (IsSameQueryLocation(query, expr->GetDebugLocation(), contestRoot)) {
                return true;
            }
        }
        if (auto terminator = block->GetTerminator();
            terminator != nullptr &&
            IsSameQueryLocation(query, terminator->GetDebugLocation(), contestRoot)) {
            return true;
        }
    }
    return false;
}

bool FunctionHasContestQueryBinding(
    const Function* func, const ContestQuery& query, const std::filesystem::path& contestRoot)
{
    if (func == nullptr || func->GetBody() == nullptr || !query.valid) {
        return false;
    }
    for (auto block : func->GetBody()->GetAllBlocks()) {
        for (auto expr : block->GetExpressions()) {
            if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
                auto debug = StaticCast<const Debug*>(expr);
                if (debug->GetSrcCodeIdentifier() == query.variableName &&
                    IsSameQueryFile(query, debug->GetDebugLocation(), contestRoot)) {
                    return true;
                }
            }
            if (query.boundGlobal != nullptr &&
                GetDirectGlobalLocation(*expr) == query.boundGlobal) {
                return true;
            }
        }
    }
    return false;
}

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
    if (IsRangePropagationPerfTraceEnabled()) {
        ++rangePropagationPerfStats.reverseCallGraphCalls;
    }
    ScopedRangePropagationPerfTimer timer(&rangePropagationPerfStats.reverseCallGraphNanos);
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
    if (IsRangePropagationPerfTraceEnabled()) {
        ++rangePropagationPerfStats.relevantFunctionCalls;
    }
    ScopedRangePropagationPerfTimer timer(&rangePropagationPerfStats.relevantFunctionNanos);
    constexpr size_t MAX_CONTEST_CONTEXT_CALL_DEPTH = 6;
    constexpr size_t MAX_CONTEST_RELEVANT_FUNCTIONS = 64;

    std::unordered_set<const Function*> relevantFunctions;
    std::vector<std::pair<const Function*, size_t>> worklist;
    for (const auto& query : queries) {
        if (!query.valid) {
            continue;
        }
        std::vector<const Function*> locationCandidates;
        std::vector<const Function*> boundCandidates;
        for (auto func : package->GetGlobalFuncsWithBody()) {
            if (!FunctionMayContainContestQuery(func, query, contestRoot)) {
                continue;
            }
            locationCandidates.emplace_back(func);
            if (FunctionHasContestQueryBinding(func, query, contestRoot)) {
                boundCandidates.emplace_back(func);
            }
        }
        const auto& selected =
            boundCandidates.empty() ? locationCandidates : boundCandidates;
        for (auto func : selected) {
            if (relevantFunctions.emplace(func).second) {
                worklist.emplace_back(func, 0);
            }
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
        rangePropagationPerfStats.maxRelevantFunctions =
            std::max(rangePropagationPerfStats.maxRelevantFunctions, relevantFunctions.size());
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
                rangePropagationPerfStats.maxRelevantFunctions =
                    std::max(rangePropagationPerfStats.maxRelevantFunctions, relevantFunctions.size());
                return relevantFunctions;
            }
            if (relevantFunctions.emplace(caller).second) {
                worklist.emplace_back(caller, depth + 1);
            }
        }
    }
    rangePropagationPerfStats.maxRelevantFunctions =
        std::max(rangePropagationPerfStats.maxRelevantFunctions, relevantFunctions.size());
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
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        auto definingExpr = value != nullptr && value->IsLocalVar()
            ? StaticCast<LocalVar*>(value)->GetExpr()
            : nullptr;
        std::cerr << "[RangeAnalysisNameBinding] name=" << debug.GetSrcCodeIdentifier()
                  << " line=" << debug.GetDebugLocation().GetBeginPos().line
                  << " value=" << value
                  << " parameter=" << (value != nullptr && value->IsParameter())
                  << " exprKind=" << (definingExpr == nullptr ? -1 : static_cast<int>(definingExpr->GetExprKind()))
                  << " exprLine=" << (definingExpr == nullptr ? 0 :
                      definingExpr->GetDebugLocation().GetBeginPos().line)
                  << '\n';
    }
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

bool IsIntegerConstantValue(Value* value)
{
    if (value == nullptr || value->GetType() == nullptr || !value->GetType()->IsInteger()) {
        return false;
    }
    auto expression = GetLocalDefiningExpression(value);
    if (expression == nullptr) {
        return false;
    }
    if (expression->GetExprKind() == ExprKind::CONSTANT) {
        return true;
    }
    if (expression->GetExprKind() != ExprKind::TYPECAST) {
        return false;
    }
    auto source = StaticCast<const TypeCast*>(expression)->GetSourceValue();
    return source != nullptr && source->GetType() != nullptr &&
        source->GetType()->IsInteger() && IsIntegerConstantValue(source);
}

bool CollectQueryValueDependencies(Value* root, std::unordered_set<const Value*>& values)
{
    constexpr size_t MAX_QUERY_DEPENDENCIES = 4096;
    // ponytail: use the existing CHIR use-def links; a capped local walk is
    // cheaper than maintaining a second interprocedural dependency graph.
    std::vector<Value*> worklist{root};
    std::unordered_set<const Value*> expanded;
    while (!worklist.empty()) {
        auto value = worklist.back();
        worklist.pop_back();
        if (value == nullptr || !expanded.emplace(value).second) {
            continue;
        }
        values.emplace(value);
        if (values.size() >= MAX_QUERY_DEPENDENCIES) {
            return false;
        }
        if (auto expression = GetLocalDefiningExpression(value); expression != nullptr) {
            for (auto operand : expression->GetOperands()) {
                auto type = operand == nullptr ? nullptr : operand->GetType();
                if (type != nullptr && (type->IsRef() || type->IsInteger() || type->IsBoolean())) {
                    worklist.emplace_back(operand);
                }
            }
        }
        auto type = value->GetType();
        if (type == nullptr || !type->IsRef()) {
            continue;
        }
        for (auto user : value->GetUsers()) {
            if (user->GetExprKind() == ExprKind::STORE &&
                StaticCast<const Store*>(user)->GetLocation() == value) {
                worklist.emplace_back(StaticCast<const Store*>(user)->GetValue());
            }
        }
    }
    return true;
}

std::unordered_set<const Block*> CollectQueryRefinementBlocks(const Ptr<const Package>& package,
    const std::vector<ContestQuery>& queries, const std::filesystem::path& contestRoot,
    std::unordered_set<const Value*>& queryValues, std::unordered_set<const Value*>& queryRoots)
{
    constexpr size_t MAX_QUERY_REFINEMENT_BLOCKS = 4096;
    std::unordered_set<const Block*> result;
    for (auto function : package->GetGlobalFuncsWithBody()) {
        if (function == nullptr || function->GetBody() == nullptr) {
            continue;
        }
        std::vector<const Block*> queryBlocks;
        auto blocks = function->GetBody()->GetAllBlocks();
        for (auto block : blocks) {
            bool containsQuery = false;
            for (auto expression : block->GetExpressions()) {
                if (expression != nullptr && expression->GetExprKind() == ExprKind::DEBUGEXPR) {
                    auto debug = StaticCast<const Debug*>(expression);
                    const bool namesQuery = std::any_of(queries.begin(), queries.end(), [&](const auto& query) {
                        return query.valid && query.variableName == debug->GetSrcCodeIdentifier() &&
                            debug->GetDebugLocation().GetBeginPos().line <= query.line &&
                            IsSameQueryFile(query, debug->GetDebugLocation(), contestRoot);
                    });
                    if (namesQuery) {
                        queryValues.emplace(debug->GetValue());
                        queryRoots.emplace(debug->GetValue());
                    }
                }
                if (expression != nullptr &&
                    MayMatchAnyContestQuery(queries, expression->GetDebugLocation(), contestRoot)) {
                    containsQuery = true;
                }
            }
            if (containsQuery) {
                queryBlocks.emplace_back(block);
            }
        }
        if (queryBlocks.empty()) {
            continue;
        }
        auto directQueryValues = queryRoots;
        for (auto value : directQueryValues) {
            if (!CollectQueryValueDependencies(const_cast<Value*>(value), queryValues)) {
                return {};
            }
        }
        std::unordered_set<const Block*> reachesQuery(queryBlocks.begin(), queryBlocks.end());
        std::vector<const Block*> worklist(queryBlocks.begin(), queryBlocks.end());
        while (!worklist.empty()) {
            auto block = worklist.back();
            worklist.pop_back();
            if (result.size() >= MAX_QUERY_REFINEMENT_BLOCKS) {
                return {};
            }
            result.emplace(block);
            for (auto predecessor : block->GetPredecessors()) {
                if (reachesQuery.emplace(predecessor).second) {
                    worklist.emplace_back(predecessor);
                }
            }
        }
    }
    return result;
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
        const bool lhsReadsLocation = IsLoadFromLocation(binary->GetLHSOperand(), location);
        const bool rhsReadsLocation = IsLoadFromLocation(binary->GetRHSOperand(), location);
        const bool hasConstantStep = storedExpression->GetExprKind() == ExprKind::ADD
            ? (lhsReadsLocation && IsIntegerConstantValue(binary->GetRHSOperand())) ||
                (rhsReadsLocation && IsIntegerConstantValue(binary->GetLHSOperand()))
            : lhsReadsLocation && IsIntegerConstantValue(binary->GetRHSOperand());
        auto storeBlock = user->GetParentBlock();
        auto reachesStore = CanReachBlockForQueryMapping(loadBlock, storeBlock);
        auto reachesLoad = CanReachBlockForQueryMapping(storeBlock, loadBlock);
        if (hasConstantStep && reachesStore == ContestReachability::REACHABLE &&
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
        if (!query.valid || query.boundGlobal != nullptr ||
            !IsSameQueryLocation(query, location, contestRoot) ||
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
    auto point = MakeContestProgramPoint(&expression, observedRange != nullptr
        ? ContestProgramPointKind::BOUNDED_LOOP_POINT
        : ContestProgramPointKind::AFTER_EXPRESSION);
    if (observedRange != nullptr && IsContestTopRange(range, type)) {
        range = observedRange.get();
    }
    auto result = FormatContestRange(range, type);
    if (!ShouldRecordContestResult(query, result, type, range, point)) {
        return;
    }
    SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
}

void ResolveQueryAtUnnamedLoopInductionOperand(std::vector<ContestQuery>& queries,
    const ValueNameMap& valueNames, const Expression& expression, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    auto queryIndex = FindUniqueUnnamedQueryAtLocation(
        queries, valueNames, expression.GetDebugLocation(), contestRoot);
    if (!queryIndex.has_value()) {
        return;
    }

    Value* candidate = nullptr;
    const Expression* candidateDefinition = nullptr;
    for (auto operand : expression.GetOperands()) {
        auto definingExpression = GetLocalDefiningExpression(operand);
        if (definingExpression == nullptr || !IsUnnamedLoopInductionLoad(*definingExpression)) {
            continue;
        }
        if (candidate != nullptr && candidate != operand) {
            return;
        }
        candidate = operand;
        candidateDefinition = definingExpression;
    }
    if (candidate == nullptr) {
        return;
    }

    auto& query = queries[*queryIndex];
    auto type = GetQueryValueType(candidate);
    auto observedRange = RangeAnalysis::GetBoundedLoopObservedRange(candidateDefinition);
    auto range = observedRange != nullptr
        ? observedRange.get()
        : GetContestRangeForValue(state, candidate);
    auto point = MakeContestProgramPoint(&expression, observedRange != nullptr
        ? ContestProgramPointKind::BOUNDED_LOOP_POINT
        : ContestProgramPointKind::AFTER_EXPRESSION);
    auto result = FormatContestRange(range, type);
    if (!ShouldRecordContestResult(query, result, type, range, point)) {
        return;
    }
    SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
}

void ResolveQueriesFromVisibleNames(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    const DebugLocation& location, const RangeDomain& state, const std::filesystem::path& contestRoot,
    bool beforeProgramPoint = false, const Expression* programExpression = nullptr)
{
    auto useScope = location.GetScopeInfo();
    for (auto& query : queries) {
        if (!query.valid || !IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        if (!beforeProgramPoint &&
            (query.programPoints.Has(ContestProgramPointKind::BOUNDED_LOOP_LIFETIME) ||
                query.programPoints.Has(ContestProgramPointKind::BOUNDED_LOOP_POINT))) {
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
                query.programPoints.hasUnknownObservation = true;
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
        auto point = MakeContestProgramPoint(programExpression, beforeProgramPoint
            ? ContestProgramPointKind::BEFORE_EXPRESSION
            : ContestProgramPointKind::AFTER_EXPRESSION);
        const bool shouldRecord = ShouldRecordContestResult(query, result, type, joined.get(), point);
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, joined.get(), ContestResultOrigin::CHIR_ANALYSIS, &point);
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
            auto point = MakeContestProgramPoint(&debug, observed != nullptr
                ? ContestProgramPointKind::BOUNDED_LOOP_POINT
                : ContestProgramPointKind::AFTER_EXPRESSION);
            if (debug.GetValue()->IsParameter() && IsContestTopRange(range, type)) {
                query.programPoints.hasUnknownObservation = true;
            }
            auto result = FormatContestRange(range, type);
            if (!ShouldRecordContestResult(query, result, type, range, point)) {
                continue;
            }
            SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
        }
    }
    ResolveQueriesFromVisibleNames(
        queries, valueNames, debug.GetDebugLocation(), state, contestRoot, false, &debug);
}

// 通过已记录的 value-name 映射解析同源码行 operand 查询。
void ResolveQueryAtValue(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames, const DebugLocation& location,
    Value* value, const RangeDomain& state, const std::filesystem::path& contestRoot,
    const Expression* programExpression, const ValueRange* boundedPointObservation = nullptr)
{
    for (auto& query : queries) {
        if (!query.valid || !HasValueNameForQuery(valueNames, value, query)) {
            continue;
        }
        if (!IsSameQueryLocation(query, location, contestRoot)) {
            continue;
        }
        auto type = GetQueryValueType(value);
        auto range = boundedPointObservation != nullptr ?
            boundedPointObservation : GetContestRangeForValue(state, value);
        auto point = MakeContestProgramPoint(programExpression, boundedPointObservation != nullptr
            ? ContestProgramPointKind::BOUNDED_LOOP_POINT
            : ContestProgramPointKind::AFTER_EXPRESSION);
        if (value->IsParameter() && IsContestTopRange(range, type)) {
            query.programPoints.hasUnknownObservation = true;
        }
        auto result = FormatContestRange(range, type);
        if (!ShouldRecordContestResult(query, result, type, range, point)) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
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

bool IsFirstExpressionForQueryInBlock(const ContestQuery& query, const Expression& expression,
    const std::filesystem::path& contestRoot)
{
    auto block = expression.GetParentBlock();
    if (block == nullptr) {
        return false;
    }
    for (auto candidate : block->GetExpressions()) {
        if (candidate == &expression) {
            return true;
        }
        if (candidate != nullptr &&
            IsSameQueryLocation(query, candidate->GetDebugLocation(), contestRoot)) {
            return false;
        }
    }
    return false;
}

void ResolveBoundGlobalQueryBeforeExpression(std::vector<ContestQuery>& queries,
    const Expression& expression, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    if (state.IsBottom()) {
        return;
    }
    for (auto& query : queries) {
        auto global = query.boundGlobal;
        if (!query.valid ||
            (query.isGlobalDeclarationQuery && !query.isUnambiguousGlobalBinding) ||
            global == nullptr ||
            !IsSameQueryLocation(query, expression.GetDebugLocation(), contestRoot) ||
            !IsFirstExpressionForQueryInBlock(query, expression, contestRoot)) {
            continue;
        }
        auto type = GetQueryValueType(global);
        auto range = GetContestRangeForValue(state, global);
        auto result = FormatContestRange(range, type);
        auto point = MakeContestProgramPoint(
            &expression, ContestProgramPointKind::BOUND_GLOBAL_PRESTATE);
        const bool shouldRecord =
            ShouldRecordContestResult(query, result, type, range, point);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisBoundGlobalBefore] query="
                      << query.variableName << '@' << query.line
                      << " exprKind=" << static_cast<int>(expression.GetExprKind())
                      << " range=" << result << '\n';
        }
        if (shouldRecord) {
            SetContestQueryResult(
                query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
        }
    }
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
        auto point = MakeContestProgramPoint(
            &expression, ContestProgramPointKind::GLOBAL_PRESTATE);
        const bool shouldRecord =
            ShouldRecordContestResult(query, result, type, range, point);
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
    }
}

constexpr size_t MAX_CONTEXT_EXACT_SINT_OBSERVATIONS = 4096;

void InvalidateExactSIntObservationUnion(ContestContextCandidate& candidate)
{
    candidate.exactSIntObservationUnionComplete = false;
    candidate.exactSIntObservations.clear();
}

void RecordExactSIntObservation(
    ContestContextCandidate& candidate, const ValueRange& range)
{
    if (!candidate.exactSIntObservationUnionComplete) {
        return;
    }
    if (range.GetRangeKind() != ValueRange::RangeKind::SINT) {
        InvalidateExactSIntObservationUnion(candidate);
        return;
    }
    const auto& domain = StaticCast<const SIntRange&>(range).GetVal();
    if (!domain.IsSingleValue() ||
        candidate.exactSIntObservations.size() >=
            MAX_CONTEXT_EXACT_SINT_OBSERVATIONS) {
        InvalidateExactSIntObservationUnion(candidate);
        return;
    }
    candidate.exactSIntObservations.emplace_back(
        domain.NumericBound().GetSingleElement());
}

std::unique_ptr<ValueRange> BuildDeterministicExactSIntObservationUnion(
    const ContestContextCandidate& candidate)
{
    if (!candidate.exactSIntObservationUnionComplete ||
        candidate.exactSIntObservations.empty() || candidate.type == nullptr ||
        !candidate.type->IsInteger()) {
        return nullptr;
    }
    auto values = candidate.exactSIntObservations;
    const bool isUnsigned = candidate.type->IsUnsignedInteger();
    std::sort(values.begin(), values.end(), [isUnsigned](const SInt& lhs, const SInt& rhs) {
        return isUnsigned ? lhs.UVal() < rhs.UVal() : lhs.SVal() < rhs.SVal();
    });
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty()) {
        return nullptr;
    }

    const auto makeSingleton = [isUnsigned](const SInt& value) {
        return std::make_unique<SIntRange>(
            SIntDomain::FromNumeric(RelationalOperation::EQ, value, isUnsigned),
            std::vector<SInt>{value});
    };
    std::unique_ptr<ValueRange> result = makeSingleton(values.front());
    for (size_t index = 1; index < values.size(); ++index) {
        auto singleton = makeSingleton(values[index]);
        if (auto joined = result->Join(*singleton); joined.has_value()) {
            result = std::move(joined.value());
        }
    }
    return result;
}

void MergeContestContextCandidate(
    ContestContextCandidateMap& candidates, size_t queryIndex, Type* type, const ValueRange* range,
    bool unknownObservation = false, bool auxiliary = false,
    bool directPointLoadObservation = false)
{
    if (type == nullptr || (range == nullptr && !unknownObservation)) {
        return;
    }
    auto& candidate = candidates[queryIndex];
    candidate.hasDirectPointLoadObservation =
        candidate.hasDirectPointLoadObservation || directPointLoadObservation;
    const bool hadObservation = candidate.range != nullptr || candidate.hasUnknownObservation;
    const bool wasAuxiliaryOnly = candidate.auxiliaryOnly;
    if (candidate.type == nullptr) {
        candidate.type = type;
    }
    if (!AreContestTypesCompatible(candidate.type, type)) {
        candidate.range = MakeContestTopRange(candidate.type);
        candidate.incompleteGlobalLifetime = true;
        InvalidateExactSIntObservationUnion(candidate);
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
        InvalidateExactSIntObservationUnion(candidate);
        candidate.range = MakeContestTopRange(candidate.type);
        return;
    }
    auto incoming = CloneContestRangeOrTop(type, range);
    if (incoming == nullptr) {
        return;
    }
    if (candidate.range == nullptr) {
        if (!auxiliary) {
            RecordExactSIntObservation(candidate, *incoming);
        }
        candidate.range = std::move(incoming);
        return;
    }
    if (auxiliary && !wasAuxiliaryOnly) {
        return;
    }
    if (!auxiliary && wasAuxiliaryOnly) {
        candidate.exactSIntObservationUnionComplete = true;
        candidate.exactSIntObservations.clear();
        RecordExactSIntObservation(candidate, *incoming);
        candidate.range = std::move(incoming);
        return;
    }
    if (candidate.range->GetRangeKind() != incoming->GetRangeKind()) {
        InvalidateExactSIntObservationUnion(candidate);
        candidate.range = MakeContestTopRange(candidate.type);
        candidate.incompleteGlobalLifetime = true;
        return;
    }
    RecordExactSIntObservation(candidate, *incoming);
    if (auto joined = candidate.range->Join(*incoming); joined.has_value()) {
        candidate.range = std::move(joined.value());
    }
}

void CollectContextCandidateAtGlobalAccess(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const Expression& expression, const RangeDomain& state,
    const std::filesystem::path& contestRoot, bool boundDeclarationsOnly = false,
    const RangeAnalysis* contextAnalysis = nullptr)
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
            auto observed = contextAnalysis == nullptr
                ? nullptr
                : contextAnalysis->GetLocalBoundedLoopObservedRange(&expression);
            if (observed == nullptr) {
                observed = RangeAnalysis::GetBoundedLoopObservedRange(&expression);
            }
            auto type = GetQueryValueType(global);
            auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, storedValue);
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                auto topLevelFunc = expression.GetTopLevelFunc();
                std::cerr << "[RangeAnalysisGlobalLifetime] kind=store query="
                          << query.variableName << '@' << query.line
                          << " global=" << static_cast<const void*>(global)
                          << " bound=" << static_cast<const void*>(query.boundGlobal)
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

void CollectContextCandidateAtGlobalEntry(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const RangeDomain& state)
{
    if (state.IsBottom()) {
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
        auto type = GetQueryValueType(global);
        auto range = GetContestRangeForValue(state, global);
        candidate.sawProgramEntryObservation = range != nullptr;
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisGlobalLifetime] kind=main-entry query="
                      << query.variableName << '@' << query.line
                      << " global=" << static_cast<const void*>(global)
                      << " range=" << FormatContestRange(range, type) << '\n';
        }
        if (range == nullptr) {
            candidate.incompleteGlobalLifetime = true;
        }
        MergeContestContextCandidate(
            candidates, index, type, range, range == nullptr);
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
                      << " global=" << static_cast<const void*>(global)
                      << " bound=" << static_cast<const void*>(query.boundGlobal)
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
                !candidate.sawInitializerObservation &&
                !candidate.sawProgramEntryObservation) ||
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
        auto point = MakeContestProgramPoint(&expr, lifetimeObservation
            ? ContestProgramPointKind::DECLARATION_LIFETIME
            : ContestProgramPointKind::DIRECT_LOAD_RESULT);
        if (observed != nullptr) {
            query.programPoints.Observe(MakeContestProgramPoint(
                &expr, ContestProgramPointKind::BOUNDED_LOOP_POINT));
        }
        if (lifetimeObservation &&
            query.programPoints.Has(ContestProgramPointKind::DECLARATION_STORE)) {
            continue;
        }
        auto result = FormatContestRange(range, type);
        if (range == nullptr) {
            query.programPoints.hasUnknownObservation = true;
        }
        const bool shouldRecord = ShouldRecordContestResult(query, result, type, range, point);
        if (!shouldRecord) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
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
        if (observed != nullptr) {
            query.programPoints.Observe(MakeContestProgramPoint(
                &expr, ContestProgramPointKind::BOUNDED_LOOP_LIFETIME));
            query.programPoints.Observe(MakeContestProgramPoint(
                &expr, ContestProgramPointKind::BOUNDED_LOOP_POINT));
        }
        auto range = observed != nullptr ? observed.get() : GetContestRangeForValue(state, store->GetValue());
        auto readModifyWrite = GetReadModifyWriteObservation(state, *store);
        const bool hadReadModifyWriteResult =
            query.programPoints.Has(ContestProgramPointKind::READ_MODIFY_WRITE);
        if (readModifyWrite.found && readModifyWrite.complete && readModifyWrite.range != nullptr) {
            query.programPoints.Observe(MakeContestProgramPoint(
                &expr, ContestProgramPointKind::READ_MODIFY_WRITE));
        }
        // A plain assignment executes after the query point and must not
        // overwrite a value already observed before that source line. Keep
        // read-modify-write observations, because they intentionally describe
        // both the value read at the point and the value written back.
        if (query.programPoints.HasBeforeResult() && !readModifyWrite.found) {
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
            query.programPoints.hasUnknownObservation = true;
        }
        auto point = MakeContestProgramPoint(&expr, readModifyWrite.found
            ? ContestProgramPointKind::READ_MODIFY_WRITE
            : ContestProgramPointKind::DECLARATION_LIFETIME);
        const bool hasCompleteBoundedPointObservation = observed != nullptr &&
            readModifyWrite.found && readModifyWrite.complete && range != nullptr &&
            !IsContestTopRange(range, type);
        if (hasCompleteBoundedPointObservation && !hadReadModifyWriteResult &&
            (!query.resolved || IsContestTopRange(query.resultRange.get(), type))) {
            SetContestQueryResult(
                query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
            continue;
        }
        if (query.programPoints.hasUnknownObservation || !readModifyWrite.complete ||
            IsContestTopRange(range, type)) {
            SetContestQueryResult(
                query, type, nullptr, ContestResultOrigin::CHIR_ANALYSIS, &point);
            continue;
        }
        if (!ShouldRecordContestResult(query, result, type, range, point)) {
            continue;
        }
        SetContestQueryResult(query, type, range, ContestResultOrigin::CHIR_ANALYSIS, &point);
        if (!readModifyWrite.found &&
            IsQueryDeclarationBinding(query, valueNames, location, store->GetDebugLocation())) {
            query.programPoints.Observe(MakeContestProgramPoint(
                &expr, ContestProgramPointKind::DECLARATION_STORE));
        }
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
                observation.kind == ContestRangeObservationKind::UNKNOWN,
                /* auxiliary = */ !value->IsParameter());
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
    if (expr.GetExprKind() == ExprKind::STORE) {
        auto store = StaticCast<const Store*>(&expr);
        for (size_t index = 0; index < queries.size(); ++index) {
            const auto& query = queries[index];
            if (!query.valid ||
                expr.GetDebugLocation().GetBeginPos().line != query.line ||
                !IsQueryDeclarationBinding(
                    query, valueNames, store->GetLocation(), expr.GetDebugLocation())) {
                continue;
            }
            auto type = GetQueryValueType(store->GetLocation());
            auto observation = ObserveContestRange(state, store->GetValue());
            if (observation.kind == ContestRangeObservationKind::ABSENT) {
                continue;
            }
            if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
                std::cerr << "[RangeAnalysisContextCandidate] kind=declaration-store query="
                          << query.variableName << '@' << query.line
                          << " range=" << FormatContestRange(observation.range, type) << '\n';
            }
            MergeContestContextCandidate(candidates, index, type, observation.range,
                observation.kind == ContestRangeObservationKind::UNKNOWN);
        }
    }
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
                    observation.kind == ContestRangeObservationKind::UNKNOWN,
                    /* auxiliary = */ false, /* directPointLoadObservation = */ true);
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
    const ValueNameMap& knownValueNames, const Expression& expr, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    // Debug expressions are emitted after the expression that defines a local
    // binding. Consult the pre-indexed map here so the unnamed-IV fallback does
    // not claim a named declaration before its Debug node is visited.
    ResolveQueryAtUnnamedLoopInductionLoad(queries, knownValueNames, expr, state, contestRoot);
    ResolveQueryAtUnnamedLoopInductionOperand(queries, knownValueNames, expr, state, contestRoot);
    ResolveQueryAtLoadResult(queries, valueNames, expr, state, contestRoot);
    ResolveQueryAtStoreValue(queries, valueNames, expr, state, contestRoot);
    std::unique_ptr<ValueRange> boundedStoredValue;
    Value* storedValue = nullptr;
    if (expr.GetExprKind() == ExprKind::STORE) {
        storedValue = StaticCast<const Store*>(&expr)->GetValue();
        boundedStoredValue = RangeAnalysis::GetBoundedLoopObservedRange(&expr);
    }
    for (auto operand : expr.GetOperands()) {
        if (expr.GetExprKind() == ExprKind::STORE &&
            operand == StaticCast<const Store*>(&expr)->GetLocation()) {
            continue;
        }
        std::unique_ptr<ValueRange> boundedDefinitionValue;
        const ValueRange* boundedPointObservation =
            operand == storedValue ? boundedStoredValue.get() : nullptr;
        if (boundedPointObservation == nullptr) {
            auto definingExpression = operand != nullptr && operand->IsLocalVar()
                ? StaticCast<const LocalVar*>(operand)->GetExpr()
                : nullptr;
            boundedDefinitionValue =
                RangeAnalysis::GetBoundedLoopObservedRange(definingExpression);
            boundedPointObservation = boundedDefinitionValue.get();
        }
        ResolveQueryAtValue(queries, valueNames, expr.GetDebugLocation(), operand, state, contestRoot,
            &expr, boundedPointObservation);
    }
    ResolveQueriesFromVisibleNames(
        queries, valueNames, expr.GetDebugLocation(), state, contestRoot, false, &expr);
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
        auto deterministicSIntUnion =
            BuildDeterministicExactSIntObservationUnion(candidate);
        const ValueRange* candidateRange = deterministicSIntUnion != nullptr
            ? deterministicSIntUnion.get()
            : candidate.range.get();
        if (query.programPoints.Has(ContestProgramPointKind::BOUND_GLOBAL_PRESTATE) &&
            query.isGlobalDeclarationQuery &&
            candidate.fromGlobalAccess) {
            // The unique global binding was only a fallback for a source line
            // without a direct GlobalVar operand. Once the caller-side CHIR
            // program point is observed, its pre-state is authoritative; the
            // declaration lifetime would mix in values from other points.
            continue;
        }
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisContextApply] query=" << query.variableName << '@' << query.line
                      << " candidate=" << FormatContestRange(candidateRange, candidate.type)
                      << " unknown=" << candidate.hasUnknownObservation
                      << " auxiliaryOnly=" << candidate.auxiliaryOnly
                      << " incomplete=" << candidate.incompleteGlobalLifetime
                      << " initializerSeen=" << candidate.sawInitializerObservation
                      << " entrySeen=" << candidate.sawProgramEntryObservation
                      << " exitSeen=" << candidate.sawExitObservation
                      << " global=" << candidate.fromGlobalAccess
                      << " rootUnknown=" << query.programPoints.hasUnknownObservation
                      << " rootRmw=" << query.programPoints.Has(
                             ContestProgramPointKind::READ_MODIFY_WRITE)
                      << " directLoad=" << candidate.hasDirectPointLoadObservation
                      << " rootTop=" << IsContestTopRange(query.resultRange.get(), query.type)
                      << " root=" << query.result << '\n';
        }
        const bool hasCompleteRootResult = query.resolved &&
            query.resultOrigin != ContestResultOrigin::NONE && query.resultRange != nullptr &&
            !query.programPoints.hasUnknownObservation &&
            !IsContestTopRange(query.resultRange.get(), query.type);
        const bool hasCompleteContextCandidate = !candidate.fromGlobalAccess &&
            !candidate.incompleteGlobalLifetime && !candidate.hasUnknownObservation &&
            !candidate.auxiliaryOnly && candidateRange != nullptr &&
            (!query.programPoints.Has(ContestProgramPointKind::READ_MODIFY_WRITE) ||
                candidate.hasDirectPointLoadObservation);
        if (!contextClosureComplete && !candidate.fromGlobalAccess) {
            // A partial call closure is useful for diagnostics but is not an
            // exhaustive set of executions. Keep the root CHIR result, which
            // is a safe over-approximation, instead of narrowing it with a
            // subset of observed contexts.
            continue;
        }
        if (hasCompleteRootResult && !candidate.fromGlobalAccess) {
            auto type = query.type == nullptr ? candidate.type : query.type;
            if (!query.programPoints.Has(ContestProgramPointKind::BOUNDED_LOOP_POINT) &&
                hasCompleteContextCandidate &&
                AreContestTypesCompatible(type, candidate.type) &&
                query.resultRange->GetRangeKind() == candidateRange->GetRangeKind()) {
                // A complete context closure partitions all reachable calls to
                // this function. Prefer that union even when a
                // context-insensitive root result is not its superset: the
                // root state may have lost aggregate alias identity at a CFG
                // join, while each normalized context still has a sound
                // memory state.
                auto point = MakeContestProgramPoint(
                    nullptr, ContestProgramPointKind::CONTEXT_SUMMARY);
                SetContestQueryResult(query, type, candidateRange,
                    ContestResultOrigin::CONTEXT_SUMMARY, &point);
            }
            continue;
        }
        if (candidate.hasUnknownObservation) {
            query.programPoints.hasUnknownObservation = true;
        }
        auto type = query.type == nullptr ? candidate.type : query.type;
        auto joined = candidate.incompleteGlobalLifetime || candidate.hasUnknownObservation
            ? MakeContestTopRange(type)
            : CloneContestRangeOrTop(candidate.type, candidateRange);
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
        auto point = MakeContestProgramPoint(
            nullptr, ContestProgramPointKind::CONTEXT_SUMMARY);
        SetContestQueryResult(
            query, type, joined.get(), ContestResultOrigin::CONTEXT_SUMMARY, &point);
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
    const auto emitStart = IsRangePropagationPerfTraceEnabled()
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
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
    ValueNameMap knownValueNames;
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
        ResolveBoundGlobalQueryBeforeExpression(
            queries.value(), *expression, state, contestRoot);
        ResolveQueryBeforeGlobalAccess(queries.value(), *expression, state, contestRoot);
        ResolveQueriesFromVisibleNames(queries.value(), valueNames,
            expression->GetDebugLocation(), state, contestRoot,
            /* beforeProgramPoint = */ true, expression);
    };
    auto actionAfterVisitExpr = [&queries, &valueNames, &knownValueNames, &aggregates, &contestRoot](
                                    const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
            ResolveQueryAtDebug(queries.value(), valueNames, aggregates, *StaticCast<Debug*>(expr), state, contestRoot);
            return;
        }
        ResolveQueryAtExpressionOperands(
            queries.value(), valueNames, knownValueNames, *expr, state, contestRoot);
    };
    const auto actionOnTerminator = [&queries, &valueNames, &knownValueNames, &contestRoot](
                                        const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
        if (terminator == nullptr) {
            return;
        }
        ResolveQueryAtExpressionOperands(
            queries.value(), valueNames, knownValueNames, *terminator, state, contestRoot);
    };
    auto resolveQueries = [&](Results<RangeDomain>& result) {
        const auto collectKnownBinding = [&knownValueNames, &contestRoot](
                                             const RangeDomain&, Expression* expr, size_t) {
            if (expr != nullptr && expr->GetExprKind() == ExprKind::DEBUGEXPR) {
                RememberValueName(
                    knownValueNames, *StaticCast<Debug*>(expr), contestRoot);
            }
        };
        const auto ignoreBefore = [](const RangeDomain&, Expression*, size_t) {};
        const auto ignoreTerminator = [](
                                          const RangeDomain&, Terminator*, std::optional<Block*>) {};
        result.VisitWith(ignoreBefore, collectKnownBinding, ignoreTerminator);
        result.VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
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
    bool contextClosureComplete = true;
    const auto visitContextResult = [&](Results<RangeDomain>& result, bool collectLifetimeExit) {
        auto contextAnalysis = dynamic_cast<RangeAnalysis*>(result.GetAnalysis());
        auto entryBlock = result.func == nullptr ? nullptr : result.func->GetEntryBlock();
        bool collectedProgramEntry = false;
        const auto contextActionBeforeVisitExpr =
            [&queries, &contextCandidates, &contestRoot, contextAnalysis,
                collectLifetimeExit, entryBlock, &collectedProgramEntry](
                const RangeDomain& state, Expression* expression, size_t expressionIndex) {
                if (collectLifetimeExit && !collectedProgramEntry && expression != nullptr &&
                    expression->GetParentBlock() == entryBlock && expressionIndex == 0) {
                    CollectContextCandidateAtGlobalEntry(
                        queries.value(), contextCandidates, state);
                    collectedProgramEntry = true;
                }
                CollectContextCandidateAtGlobalAccess(
                    queries.value(), contextCandidates, *expression, state, contestRoot,
                    /* boundDeclarationsOnly = */ false, contextAnalysis);
            };
        const auto contextActionOnTerminator =
            [&queries, &contextCandidates, &collectContextTerminator,
                collectLifetimeExit, entryBlock, &collectedProgramEntry](
                const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
                if (collectLifetimeExit && !collectedProgramEntry && terminator != nullptr &&
                    terminator->GetParentBlock() == entryBlock) {
                    CollectContextCandidateAtGlobalEntry(
                        queries.value(), contextCandidates, state);
                    collectedProgramEntry = true;
                }
                collectContextTerminator(state, terminator, collectLifetimeExit);
            };
        result.VisitFunctionWith(
            contextActionBeforeVisitExpr, contextActionAfterVisitExpr,
            contextActionOnTerminator);
        if (contextAnalysis != nullptr) {
            const auto contextLambdaActionOnTerminator =
                [&collectContextTerminator](const RangeDomain& state, Terminator* terminator,
                    std::optional<Block*>) {
                    collectContextTerminator(
                        state, terminator, /* collectLifetimeExit = */ false);
                };
            contextAnalysis->VisitContextSensitiveLambdaResults(
                contextActionBeforeVisitExpr, contextActionAfterVisitExpr,
                contextLambdaActionOnTerminator);
        }
    };
    auto collectContextCandidates = [&](const Function* rootFunction, Results<RangeDomain>& root) {
        const bool collectLifetimeExit = rootFunction != nullptr &&
            rootFunction->GetFuncKind() == FuncKind::MAIN_ENTRY;
        visitContextResult(root, collectLifetimeExit);
        contextClosureComplete = CollectContestContextLoopObservations(
            contextLoopObservations, root) && contextClosureComplete;
        contextClosureComplete = RangeAnalysis::VisitReachableContextSensitiveResults(rootFunction, root,
            [&](const Function*, const std::string&, Results<RangeDomain>& result) {
                visitContextResult(result, /* collectLifetimeExit = */ false);
                contextClosureComplete =
                    CollectContestContextLoopObservations(
                        contextLoopObservations, result) &&
                    contextClosureComplete;
            }) && contextClosureComplete;
    };

    // The contest path does not call AnalysisWrapper::RunOnPackage(), so its
    // normal global-state setup would otherwise be skipped. Analyse each
    // tracked readonly initializer before main and let ValueAnalysis populate
    // the package-wide abstract global state from CHIR stores.
    RangeAnalysis::ClearContextSensitiveResults();
    std::unordered_set<const Function*> analysedReadonlyInitializers;
    for (auto global : package->GetGlobalVarsWithInit()) {
        if (global == nullptr || !IsGlobalVarInCurrentPackage(global) ||
            !global->TestAttr(Attribute::READONLY) ||
            !IsTrackedGV<RangeValueDomain>(*global)) {
            continue;
        }
        auto initializer = global->GetInitFunc();
        if (initializer == nullptr || initializer->GetBody() == nullptr ||
            !analysedReadonlyInitializers.emplace(initializer).second) {
            continue;
        }
        auto result = rangeAnalysisWrapper.RunOnFunc(initializer, /* isDebug = */ false, diag);
        if (result != nullptr) {
            resolveQueries(*result);
            visitContextResult(*result, /* collectLifetimeExit = */ false);
        }
    }

    auto relevantFunctions = CollectContestRelevantFunctions(package, queries.value(), contestRoot);
    auto reverseCallGraph = BuildReverseContestCallGraph(package);
    rangePropagationPerfStats.maxPackageFunctions = std::max(
        rangePropagationPerfStats.maxPackageFunctions, package->GetGlobalFuncsWithBody().size());
    const bool hasMainContextRoot = std::any_of(
        relevantFunctions.begin(), relevantFunctions.end(), [](const auto* function) {
            return function != nullptr && function->GetFuncKind() == FuncKind::MAIN_ENTRY;
        });
    auto packageInit = package->GetPackageInitFunc();

    RangeAnalysis::ClearQueryRefinementBlocks();
    if (std::getenv("CANGJIE_RA_DISABLE_QGSR") == nullptr) {
        std::unordered_set<const Value*> refinementValues;
        std::unordered_set<const Value*> refinementRoots;
        auto refinementBlocks =
            CollectQueryRefinementBlocks(
                package, queries.value(), contestRoot, refinementValues, refinementRoots);
        if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
            std::cerr << "[RangeAnalysisQGSR] selectedBlocks=" << refinementBlocks.size()
                      << " protectedValues=" << refinementValues.size()
                      << " queryRoots=" << refinementRoots.size() << '\n';
        }
        RangeAnalysis::SetQueryRefinementContext(std::move(refinementBlocks),
            std::move(refinementValues), std::move(refinementRoots));
    }
    RangeAnalysis::ClearContextSensitiveResults();
    size_t contextRootCount = 0;
    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (!RangeAnalysis::Filter(*func) || relevantFunctions.find(func) == relevantFunctions.end()) {
            continue;
        }
        auto callers = reverseCallGraph.find(func);
        const bool hasDirectCaller =
            callers != reverseCallGraph.end() && !callers->second.empty();
        const bool isContextRoot = func->GetFuncKind() == FuncKind::MAIN_ENTRY ||
            func == packageInit || (!hasMainContextRoot && !hasDirectCaller);
        contextRootCount += isContextRoot ? 1 : 0;
        if (IsRangePropagationPerfTraceEnabled()) {
            ++rangePropagationPerfStats.runFunctionCalls;
        }
        std::unique_ptr<Results<RangeDomain>> result;
        {
            ScopedRangePropagationPerfTimer timer(&rangePropagationPerfStats.runFunctionNanos);
            result = rangeAnalysisWrapper.RunOnFunc(func, /* isDebug = */ false, diag);
        }
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
    rangePropagationPerfStats.maxContextRoots =
        std::max(rangePropagationPerfStats.maxContextRoots, contextRootCount);

    FinalizeBoundGlobalCandidateCompleteness(
        queries.value(), contextCandidates, hasMainContextRoot);
    ApplyContestContextCandidates(
        queries.value(), contextCandidates, contextClosureComplete);
    ApplyContestAggregates(
        queries.value(), aggregates, contextLoopObservations,
        contextClosureComplete);
    if (std::getenv("CANGJIE_RA_TRACE_OUTPUT") != nullptr) {
        std::cerr << "[RangeAnalysisContextClosure] complete="
                  << contextClosureComplete << '\n';
    }
    WriteContestOutput(queries.value(), inputContext.value());
    RangeAnalysis::ClearQueryRefinementBlocks();
    if (IsRangePropagationPerfTraceEnabled()) {
        rangePropagationPerfStats.emitNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - emitStart).count());
    }
    PrintAndResetRangePropagationPerfStats();
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
