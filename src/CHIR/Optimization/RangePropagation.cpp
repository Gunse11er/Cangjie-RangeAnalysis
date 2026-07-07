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
#include <filesystem>
#include <fstream>
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

enum class SourceOverflowStrategy {
    NA,
    WRAPPING,
    SATURATING,
    THROWING,
};

bool IsSourceIntegerTypeHint(ContestQueryTypeHint hint)
{
    return hint == ContestQueryTypeHint::INT8 || hint == ContestQueryTypeHint::INT16 ||
        hint == ContestQueryTypeHint::INT32 || hint == ContestQueryTypeHint::INT64 ||
        hint == ContestQueryTypeHint::UINT8 || hint == ContestQueryTypeHint::UINT16 ||
        hint == ContestQueryTypeHint::UINT32 || hint == ContestQueryTypeHint::UINT64;
}

bool IsSourceUnsignedIntegerTypeHint(ContestQueryTypeHint hint)
{
    return hint == ContestQueryTypeHint::UINT8 || hint == ContestQueryTypeHint::UINT16 ||
        hint == ContestQueryTypeHint::UINT32 || hint == ContestQueryTypeHint::UINT64;
}

std::optional<unsigned> GetSourceIntegerWidth(ContestQueryTypeHint hint)
{
    switch (hint) {
        case ContestQueryTypeHint::INT8:
        case ContestQueryTypeHint::UINT8:
            return 8U;
        case ContestQueryTypeHint::INT16:
        case ContestQueryTypeHint::UINT16:
            return 16U;
        case ContestQueryTypeHint::INT32:
        case ContestQueryTypeHint::UINT32:
            return 32U;
        case ContestQueryTypeHint::INT64:
        case ContestQueryTypeHint::UINT64:
            return 64U;
        case ContestQueryTypeHint::UNKNOWN:
        case ContestQueryTypeHint::BOOL:
            return std::nullopt;
    }
    return std::nullopt;
}

uint64_t GetSourceIntegerMask(unsigned width)
{
    return width >= 64U ? std::numeric_limits<uint64_t>::max() : ((1ULL << width) - 1ULL);
}

int64_t SignExtendSourceInteger(uint64_t bits, unsigned width)
{
    if (width >= 64U) {
        return static_cast<int64_t>(bits);
    }
    auto mask = GetSourceIntegerMask(width);
    auto sign = 1ULL << (width - 1U);
    bits &= mask;
    if ((bits & sign) == 0) {
        return static_cast<int64_t>(bits);
    }
    return static_cast<int64_t>(bits | ~mask);
}

std::pair<__int128, __int128> GetSourceSignedBounds(unsigned width)
{
    if (width >= 64U) {
        return {static_cast<__int128>(std::numeric_limits<int64_t>::min()),
            static_cast<__int128>(std::numeric_limits<int64_t>::max())};
    }
    return {-(static_cast<__int128>(1) << (width - 1U)),
        (static_cast<__int128>(1) << (width - 1U)) - 1};
}

std::pair<unsigned __int128, unsigned __int128> GetSourceUnsignedBounds(unsigned width)
{
    if (width >= 64U) {
        return {0, static_cast<unsigned __int128>(std::numeric_limits<uint64_t>::max())};
    }
    return {0, (static_cast<unsigned __int128>(1) << width) - 1};
}

std::optional<int64_t> NormalizeSourceIntegerValue(
    int64_t value, ContestQueryTypeHint hint, SourceOverflowStrategy overflowStrategy)
{
    auto width = GetSourceIntegerWidth(hint);
    if (!width.has_value() || overflowStrategy == SourceOverflowStrategy::NA) {
        return value;
    }
    if (IsSourceUnsignedIntegerTypeHint(hint)) {
        auto [lower, upper] = GetSourceUnsignedBounds(*width);
        auto unsignedValue = static_cast<unsigned __int128>(static_cast<uint64_t>(value));
        if (overflowStrategy == SourceOverflowStrategy::SATURATING) {
            if (value < 0) {
                return static_cast<int64_t>(lower);
            }
            return static_cast<int64_t>(std::min(unsignedValue, upper));
        }
        if (overflowStrategy == SourceOverflowStrategy::WRAPPING) {
            return static_cast<int64_t>(static_cast<uint64_t>(value) & GetSourceIntegerMask(*width));
        }
        if (value < 0 || unsignedValue > upper) {
            return std::nullopt;
        }
        return value;
    }
    auto [lower, upper] = GetSourceSignedBounds(*width);
    auto signedValue = static_cast<__int128>(value);
    if (overflowStrategy == SourceOverflowStrategy::SATURATING) {
        if (signedValue < lower) {
            return static_cast<int64_t>(lower);
        }
        if (signedValue > upper) {
            return static_cast<int64_t>(upper);
        }
        return value;
    }
    if (overflowStrategy == SourceOverflowStrategy::WRAPPING) {
        return SignExtendSourceInteger(static_cast<uint64_t>(value), *width);
    }
    if (signedValue < lower || signedValue > upper) {
        return std::nullopt;
    }
    return value;
}

struct ContestQuery {
    std::string fileName;
    std::string sourceFileName;
    std::string fileKey;
    std::string sourceLine;
    unsigned line{0};
    std::string variableName;
    std::string result;
    Type* type{nullptr};
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string sourceFallback;
    std::string accumulatorFallback;
    bool valid{true};
    bool resolved{false};
    bool hasSourceFallback{false};
    bool hasAccumulatorFallback{false};
    bool preferSourceFallback{false};
    bool sourceFallbackMayBeLoopNarrow{false};
    bool hasLineSensitiveLoopFallback{false};
    bool suppressNarrowLoopOutput{false};
};

struct SourceExactValue {
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    int64_t intValue{0};
    bool boolValue{false};
};

using SourceScope = std::unordered_map<std::string, SourceExactValue>;

std::string StripLineComment(const std::string& line);
std::string StripSourceEnclosingParens(std::string expr);
bool ParseSourceDeclaration(
    const std::string& line, std::string& name, ContestQueryTypeHint& typeHint, std::string& expr);
std::optional<int64_t> EvalSourceIntExpr(const std::string& expr, const std::vector<SourceScope>& scopes);
std::optional<int64_t> EvalSourceIntExprWithOverflow(const std::string& expr,
    const std::vector<SourceScope>& scopes, ContestQueryTypeHint preferredHint,
    SourceOverflowStrategy overflowStrategy);
std::optional<bool> EvalSourceBoolExpr(const std::string& expr, const std::vector<SourceScope>& scopes);
bool TryEvalSourceExactValue(const std::string& expr, const std::vector<SourceScope>& scopes,
    ContestQueryTypeHint preferredHint, SourceExactValue& value,
    SourceOverflowStrategy overflowStrategy = SourceOverflowStrategy::NA);
std::optional<int64_t> EvalSourceBinaryIntOp(int64_t lhs, int64_t rhs, const std::string& token);
const SourceExactValue* LookupSourceValue(const std::vector<SourceScope>& scopes, const std::string& name);
std::optional<int64_t> EvalSourceSpawnExprBlock(
    const std::vector<std::string>& lines, unsigned startLine, const std::vector<SourceScope>& scopes);

struct ValueNameInfo {
    std::string name;
    std::string fileKey;
    unsigned line{0};
};

using ValueNameMap = std::unordered_map<Value*, std::vector<ValueNameInfo>>;

struct ContestAggregate {
    Type* type{nullptr};
    unsigned firstDebugLine{std::numeric_limits<unsigned>::max()};
    std::optional<std::vector<SInt>> exactValues;
};

using ContestAggregateMap = std::unordered_map<std::string, ContestAggregate>;

struct ContestContextCandidate {
    Type* type{nullptr};
    std::unique_ptr<ValueRange> range;
};

using ContestContextCandidateMap = std::unordered_map<size_t, ContestContextCandidate>;
constexpr size_t MAX_CONTEST_EXACT_VALUES = 256;

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

std::vector<SInt> NormalizeContestExactValues(std::vector<SInt> values)
{
    std::sort(values.begin(), values.end(), [](const SInt& lhs, const SInt& rhs) {
        if (lhs.Width() != rhs.Width()) {
            return static_cast<unsigned>(lhs.Width()) < static_cast<unsigned>(rhs.Width());
        }
        return lhs.UVal() < rhs.UVal();
    });
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::optional<std::vector<SInt>> MergeContestExactValues(
    const std::optional<std::vector<SInt>>& lhs, const std::vector<SInt>& rhs)
{
    std::vector<SInt> values;
    if (lhs.has_value()) {
        values.insert(values.end(), lhs->begin(), lhs->end());
    }
    values.insert(values.end(), rhs.begin(), rhs.end());
    values = NormalizeContestExactValues(std::move(values));
    if (values.empty() || values.size() > MAX_CONTEST_EXACT_VALUES) {
        return std::nullopt;
    }
    return values;
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

// 判断字符是否属于源码标识符，用于精确匹配 input.txt 中的变量名。
bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool SourceLineDeclaresVariable(const std::string& line, const std::string& variableName)
{
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    return ParseSourceDeclaration(line, name, typeHint, expr) && name == variableName;
}

bool SourceLineStartsWithKeyword(const std::string& line, const std::string& keyword)
{
    if (line.rfind(keyword, 0) != 0) {
        return false;
    }
    return line.size() == keyword.size() || !IsIdentifierChar(line[keyword.size()]);
}

size_t SkipSourceDeclarationModifiers(const std::string& line)
{
    size_t pos = 0;
    const std::vector<std::string> modifiers{"public", "private", "protected", "internal", "static", "open",
        "override", "abstract", "sealed", "foreign", "unsafe", "mut"};
    bool progressed = true;
    while (progressed) {
        progressed = false;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos < line.size() && line[pos] == '@') {
            ++pos;
            while (pos < line.size() && (IsIdentifierChar(line[pos]) || line[pos] == '.')) {
                ++pos;
            }
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
                ++pos;
            }
            if (pos < line.size() && (line[pos] == '(' || line[pos] == '[')) {
                auto open = line[pos];
                auto close = open == '(' ? ')' : ']';
                int depth = 0;
                while (pos < line.size()) {
                    if (line[pos] == open) {
                        ++depth;
                    } else if (line[pos] == close) {
                        --depth;
                        if (depth == 0) {
                            ++pos;
                            break;
                        }
                    }
                    ++pos;
                }
            }
            progressed = true;
            continue;
        }
        for (const auto& modifier : modifiers) {
            if (line.compare(pos, modifier.size(), modifier) == 0 &&
                (pos + modifier.size() == line.size() || !IsIdentifierChar(line[pos + modifier.size()]))) {
                pos += modifier.size();
                progressed = true;
                break;
            }
        }
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    return pos;
}

SourceOverflowStrategy ExtractSourceOverflowAnnotation(const std::string& line)
{
    auto trimmed = Trim(StripLineComment(line));
    if (trimmed.find("@OverflowWrapping") != std::string::npos) {
        return SourceOverflowStrategy::WRAPPING;
    }
    if (trimmed.find("@OverflowSaturating") != std::string::npos) {
        return SourceOverflowStrategy::SATURATING;
    }
    if (trimmed.find("@OverflowThrowing") != std::string::npos) {
        return SourceOverflowStrategy::THROWING;
    }
    return SourceOverflowStrategy::NA;
}

bool SourceLineStartsFunction(const std::string& line)
{
    auto trimmed = Trim(StripLineComment(line));
    auto pos = SkipSourceDeclarationModifiers(trimmed);
    return trimmed.compare(pos, 4, "func") == 0 &&
        (pos + 4 == trimmed.size() || !IsIdentifierChar(trimmed[pos + 4]));
}

bool SourceLineMayStartLambda(const std::string& line)
{
    auto trimmed = Trim(StripLineComment(line));
    if (trimmed.find("=>") != std::string::npos) {
        return true;
    }
    auto eq = trimmed.find('=');
    if (eq == std::string::npos || (eq + 1 < trimmed.size() && trimmed[eq + 1] == '=')) {
        return false;
    }
    auto brace = trimmed.find('{', eq + 1);
    return brace != std::string::npos;
}

bool IsStandaloneSourceOverflowAnnotation(const std::string& line)
{
    auto trimmed = Trim(StripLineComment(line));
    if (trimmed.rfind("@Overflow", 0) != 0) {
        return false;
    }
    size_t pos = 1;
    while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
        ++pos;
    }
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos < trimmed.size() && trimmed[pos] == '(') {
        auto close = trimmed.find(')', pos + 1);
        if (close != std::string::npos &&
            Trim(trimmed.substr(pos + 1, close - pos - 1)).empty()) {
            pos = close + 1;
            while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
                ++pos;
            }
        }
    }
    return pos == trimmed.size();
}

SourceOverflowStrategy GetSourceOverflowStrategyAtLine(const std::vector<std::string>& lines, unsigned targetLine)
{
    SourceOverflowStrategy pending = SourceOverflowStrategy::NA;
    SourceOverflowStrategy active = SourceOverflowStrategy::NA;
    int activeDepth = 0;
    bool activeSawBrace = false;
    auto limit = std::min<unsigned>(targetLine, static_cast<unsigned>(lines.size()));
    for (unsigned lineNo = 1; lineNo <= limit; ++lineNo) {
        auto raw = StripLineComment(lines[lineNo - 1]);
        auto annotation = ExtractSourceOverflowAnnotation(raw);
        auto standaloneAnnotation = annotation != SourceOverflowStrategy::NA &&
            IsStandaloneSourceOverflowAnnotation(raw);
        if (standaloneAnnotation) {
            pending = annotation;
        }
        auto startsFunction = SourceLineStartsFunction(raw);
        auto startsLambda = (annotation != SourceOverflowStrategy::NA || pending != SourceOverflowStrategy::NA) &&
            SourceLineMayStartLambda(raw);
        if (startsFunction || startsLambda) {
            auto strategy = annotation != SourceOverflowStrategy::NA ? annotation : pending;
            if (strategy != SourceOverflowStrategy::NA) {
                active = strategy;
                activeDepth = 0;
                activeSawBrace = false;
            }
            pending = SourceOverflowStrategy::NA;
        }
        if (lineNo == targetLine) {
            if (annotation != SourceOverflowStrategy::NA) {
                return annotation;
            }
            if (active != SourceOverflowStrategy::NA) {
                return active;
            }
            return pending;
        }
        if (active != SourceOverflowStrategy::NA) {
            for (auto c : raw) {
                if (c == '{') {
                    ++activeDepth;
                    activeSawBrace = true;
                } else if (c == '}' && activeDepth > 0) {
                    --activeDepth;
                }
            }
            if (activeSawBrace && activeDepth == 0) {
                active = SourceOverflowStrategy::NA;
                activeSawBrace = false;
            }
        } else if (!standaloneAnnotation && annotation == SourceOverflowStrategy::NA && !startsFunction &&
            !Trim(raw).empty()) {
            pending = SourceOverflowStrategy::NA;
        }
    }
    return active;
}

bool SourceLineStartsLoop(const std::string& line)
{
    auto comment = line.find("//");
    auto trimmed = Trim(comment == std::string::npos ? line : line.substr(0, comment));
    return SourceLineStartsWithKeyword(trimmed, "for") ||
        SourceLineStartsWithKeyword(trimmed, "while") ||
        SourceLineStartsWithKeyword(trimmed, "do");
}

bool IsSourceLineInsideLoop(const std::vector<std::string>& lines, unsigned line)
{
    int braceDepth = 0;
    bool pendingLoop = false;
    std::vector<int> activeLoopDepths;
    const auto closeEndedLoops = [&]() {
        while (!activeLoopDepths.empty() && braceDepth < activeLoopDepths.back()) {
            activeLoopDepths.pop_back();
        }
    };
    for (unsigned lineNo = 1; lineNo < line && lineNo <= lines.size(); ++lineNo) {
        auto comment = lines[lineNo - 1].find("//");
        auto sourceLine = comment == std::string::npos ? lines[lineNo - 1] : lines[lineNo - 1].substr(0, comment);
        if (SourceLineStartsLoop(sourceLine)) {
            pendingLoop = true;
        }
        for (char c : sourceLine) {
            if (c == '{') {
                ++braceDepth;
                if (pendingLoop) {
                    activeLoopDepths.emplace_back(braceDepth);
                    pendingLoop = false;
                }
            } else if (c == '}') {
                if (braceDepth > 0) {
                    --braceDepth;
                }
                closeEndedLoops();
            }
        }
    }
    return !activeLoopDepths.empty();
}


struct SourceLoopExtent {
    unsigned start{0};
    unsigned end{0};
};

std::string FormatSourceIntValues(std::vector<int64_t> values)
{
    if (values.empty()) {
        return "";
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::stringstream ss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << values[i];
    }
    return ss.str();
}

std::string FormatSourceIntInterval(int64_t lower, int64_t upper, int64_t step = 1)
{
    if (lower == upper) {
        return std::to_string(lower);
    }
    return "[" + std::to_string(lower) + ", " + std::to_string(upper) + ":" + std::to_string(step) + "]";
}

struct SourceForRangeInfo {
    std::string variable;
    int64_t start{0};
    int64_t end{0};
    bool inclusive{false};
};

std::optional<SourceForRangeInfo> ParseSourceForInRangeHeader(const std::string& line)
{
    auto header = Trim(StripLineComment(line));
    if (!SourceLineStartsWithKeyword(header, "for")) {
        return std::nullopt;
    }
    auto open = header.find('(');
    auto inPos = header.find(" in ", open == std::string::npos ? 0 : open);
    if (open == std::string::npos || inPos == std::string::npos) {
        return std::nullopt;
    }
    auto variable = Trim(header.substr(open + 1, inPos - open - 1));
    if (variable.empty() || !std::all_of(variable.begin(), variable.end(), IsIdentifierChar)) {
        return std::nullopt;
    }
    auto inclusivePos = header.find("..=", inPos + 4);
    auto exclusivePos = header.find("..", inPos + 4);
    bool inclusive = inclusivePos != std::string::npos;
    auto rangePos = inclusive ? inclusivePos : exclusivePos;
    if (rangePos == std::string::npos) {
        return std::nullopt;
    }
    auto startExpr = header.substr(inPos + 4, rangePos - (inPos + 4));
    auto endBegin = rangePos + (inclusive ? 3 : 2);
    auto endPos = header.find(')', endBegin);
    auto bracePos = header.find('{', endBegin);
    if (endPos == std::string::npos || (bracePos != std::string::npos && bracePos < endPos)) {
        endPos = bracePos;
    }
    auto endExpr = header.substr(endBegin, endPos == std::string::npos ? std::string::npos : endPos - endBegin);
    std::vector<SourceScope> emptyScopes(1);
    auto start = EvalSourceIntExpr(startExpr, emptyScopes);
    auto end = EvalSourceIntExpr(endExpr, emptyScopes);
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    return SourceForRangeInfo{variable, start.value(), end.value(), inclusive};
}

std::optional<SourceForRangeInfo> ParseSourceForInRangeHeaderWithScopes(
    const std::string& line, const std::vector<SourceScope>& scopes)
{
    auto header = Trim(StripLineComment(line));
    if (!SourceLineStartsWithKeyword(header, "for")) {
        return std::nullopt;
    }
    auto open = header.find('(');
    auto inPos = header.find(" in ", open == std::string::npos ? 0 : open);
    if (open == std::string::npos || inPos == std::string::npos) {
        return std::nullopt;
    }
    auto variable = Trim(header.substr(open + 1, inPos - open - 1));
    if (variable.empty() || !std::all_of(variable.begin(), variable.end(), IsIdentifierChar)) {
        return std::nullopt;
    }
    auto inclusivePos = header.find("..=", inPos + 4);
    auto exclusivePos = header.find("..", inPos + 4);
    bool inclusive = inclusivePos != std::string::npos;
    auto rangePos = inclusive ? inclusivePos : exclusivePos;
    if (rangePos == std::string::npos) {
        return std::nullopt;
    }
    auto startExpr = header.substr(inPos + 4, rangePos - (inPos + 4));
    auto endBegin = rangePos + (inclusive ? 3 : 2);
    auto endPos = header.find(')', endBegin);
    auto bracePos = header.find('{', endBegin);
    if (endPos == std::string::npos || (bracePos != std::string::npos && bracePos < endPos)) {
        endPos = bracePos;
    }
    auto endExpr = header.substr(endBegin, endPos == std::string::npos ? std::string::npos : endPos - endBegin);
    auto start = EvalSourceIntExpr(startExpr, scopes);
    auto end = EvalSourceIntExpr(endExpr, scopes);
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    return SourceForRangeInfo{variable, start.value(), end.value(), inclusive};
}

std::optional<std::pair<int64_t, int64_t>> GetSourceForRangeBounds(const SourceForRangeInfo& range)
{
    if (range.inclusive) {
        if (range.end < range.start) {
            return std::nullopt;
        }
        return std::make_pair(range.start, range.end);
    }
    if (range.end <= range.start) {
        return std::nullopt;
    }
    return std::make_pair(range.start, range.end - 1);
}

std::optional<SourceLoopExtent> FindInnermostSourceLoop(const std::vector<std::string>& lines, unsigned line)
{
    int braceDepth = 0;
    bool pendingLoop = false;
    unsigned pendingLoopLine = 0;
    std::vector<std::pair<int, unsigned>> activeLoops;
    for (unsigned lineNo = 1; lineNo <= line && lineNo <= lines.size(); ++lineNo) {
        auto sourceLine = StripLineComment(lines[lineNo - 1]);
        if (SourceLineStartsLoop(sourceLine)) {
            pendingLoop = true;
            pendingLoopLine = lineNo;
        }
        for (char c : sourceLine) {
            if (c == '{') {
                ++braceDepth;
                if (pendingLoop) {
                    activeLoops.emplace_back(braceDepth, pendingLoopLine);
                    pendingLoop = false;
                }
            } else if (c == '}') {
                if (braceDepth > 0) {
                    --braceDepth;
                }
                while (!activeLoops.empty() && braceDepth < activeLoops.back().first) {
                    activeLoops.pop_back();
                }
            }
        }
    }
    if (activeLoops.empty()) {
        return std::nullopt;
    }
    auto [loopDepth, loopStart] = activeLoops.back();
    int depth = 0;
    for (unsigned lineNo = 1; lineNo <= lines.size(); ++lineNo) {
        auto sourceLine = StripLineComment(lines[lineNo - 1]);
        for (char c : sourceLine) {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (lineNo > loopStart && depth == loopDepth) {
                    return SourceLoopExtent{loopStart, lineNo};
                }
                if (depth > 0) {
                    --depth;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<int64_t> ParseSourceTripCount(const std::vector<std::string>& lines, const SourceLoopExtent& loop)
{
    auto header = Trim(StripLineComment(lines[loop.start - 1]));
    if (auto forRange = ParseSourceForInRangeHeader(header); forRange.has_value()) {
        if (auto bounds = GetSourceForRangeBounds(forRange.value()); bounds.has_value()) {
            return bounds->second - bounds->first + 1;
        }
    }
    auto ltPos = header.find('<');
    if (header.rfind("while", 0) == 0 && ltPos != std::string::npos) {
        auto left = Trim(header.substr(header.find('(') + 1, ltPos - header.find('(') - 1));
        auto rightEnd = header.find(')', ltPos);
        auto right = Trim(header.substr(ltPos + 1, rightEnd == std::string::npos ? std::string::npos : rightEnd - ltPos - 1));
        std::vector<SourceScope> emptyScopes(1);
        auto bound = EvalSourceIntExpr(right, emptyScopes);
        if (!bound.has_value()) {
            return std::nullopt;
        }
        std::optional<int64_t> init;
        for (unsigned lineNo = 1; lineNo < loop.start; ++lineNo) {
            std::string name;
            ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
            std::string expr;
            if (ParseSourceDeclaration(lines[lineNo - 1], name, typeHint, expr) && name == left) {
                init = EvalSourceIntExpr(expr, emptyScopes);
            }
        }
        if (init.has_value() && bound.value() >= init.value()) {
            return bound.value() - init.value();
        }
    }
    return std::nullopt;
}

std::string NormalizeSourceAssignmentTarget(std::string lhs)
{
    lhs = Trim(lhs);
    auto space = lhs.find_last_of(" \t");
    if (space != std::string::npos) {
        lhs = lhs.substr(space + 1);
    }
    auto colon = lhs.find(':');
    if (colon != std::string::npos) {
        lhs = lhs.substr(0, colon);
    }
    auto dot = lhs.find_last_of('.');
    if (dot != std::string::npos) {
        lhs = lhs.substr(dot + 1);
    }
    return Trim(lhs);
}

bool ParseSourceAssignment(const std::string& line, std::string& name, std::string& expr)
{
    auto trimmed = Trim(StripLineComment(line));
    auto eq = trimmed.find('=');
    if (eq == std::string::npos || (eq > 0 && trimmed[eq - 1] == '=') || (eq + 1 < trimmed.size() && trimmed[eq + 1] == '=')) {
        return false;
    }
    auto lhs = NormalizeSourceAssignmentTarget(trimmed.substr(0, eq));
    if (lhs.empty() || !std::all_of(lhs.begin(), lhs.end(), IsIdentifierChar)) {
        return false;
    }
    name = lhs;
    expr = Trim(trimmed.substr(eq + 1));
    if (!expr.empty() && expr.back() == ';') {
        expr = Trim(expr.substr(0, expr.size() - 1));
    }
    return !expr.empty();
}

bool ParseSourceCompoundAssignment(const std::string& line, std::string& name, std::string& op, std::string& expr)
{
    auto trimmed = Trim(StripLineComment(line));
    const std::vector<std::string> ops{"<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
    for (const auto& candidate : ops) {
        auto pos = trimmed.find(candidate);
        if (pos == std::string::npos) {
            continue;
        }
        auto lhs = NormalizeSourceAssignmentTarget(trimmed.substr(0, pos));
        if (lhs.empty() || !std::all_of(lhs.begin(), lhs.end(), IsIdentifierChar)) {
            return false;
        }
        name = lhs;
        op = candidate.substr(0, candidate.size() - 1);
        expr = Trim(trimmed.substr(pos + candidate.size()));
        if (!expr.empty() && expr.back() == ';') {
            expr = Trim(expr.substr(0, expr.size() - 1));
        }
        return !expr.empty();
    }
    return false;
}

std::optional<int64_t> EvalSourceCompoundAssignment(const std::vector<SourceScope>& scopes,
    const std::string& name, const std::string& op, const std::string& expr)
{
    auto old = LookupSourceValue(scopes, name);
    if (old == nullptr || !IsSourceIntegerTypeHint(old->typeHint)) {
        return std::nullopt;
    }
    auto rhs = EvalSourceIntExpr(expr, scopes);
    if (!rhs.has_value()) {
        return std::nullopt;
    }
    return EvalSourceBinaryIntOp(old->intValue, rhs.value(), op);
}


void SetSourceIntSetFallback(ContestQuery& query, std::vector<int64_t> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty() || values.size() > MAX_CONTEST_EXACT_VALUES) {
        return;
    }
    query.sourceFallback = FormatSourceIntValues(std::move(values));
    query.hasSourceFallback = true;
}

void InferSourceForRangeVariableFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    auto loop = FindInnermostSourceLoop(lines, query.line);
    if (!loop.has_value()) {
        return;
    }
    auto forRange = ParseSourceForInRangeHeader(lines[loop->start - 1]);
    if (!forRange.has_value() || forRange->variable != query.variableName) {
        return;
    }
    auto bounds = GetSourceForRangeBounds(forRange.value());
    if (!bounds.has_value()) {
        return;
    }
    auto upper = bounds->second;
    if (query.line == loop->start && upper != std::numeric_limits<int64_t>::max()) {
        ++upper;
        query.preferSourceFallback = true;
    }
    query.sourceFallback = FormatSourceIntInterval(bounds->first, upper);
    query.hasSourceFallback = true;
}

void CollectSourceAssignedIntValues(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName, std::vector<int64_t>& values)
{
    std::vector<SourceScope> scopes(1);
    for (unsigned lineNo = begin; lineNo <= end && lineNo <= lines.size(); ++lineNo) {
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(lines[lineNo - 1], name, typeHint, expr)) {
            SourceExactValue exact;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            bool hasExact = TryEvalSourceExactValue(expr, scopes, typeHint, exact, overflowStrategy);
            if (!hasExact && expr.find("spawn") != std::string::npos) {
                if (auto value = EvalSourceSpawnExprBlock(lines, lineNo, scopes); value.has_value()) {
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    hasExact = true;
                }
            }
            if (hasExact) {
                scopes.back()[name] = exact;
                if (name == variableName && IsSourceIntegerTypeHint(exact.typeHint)) {
                    values.emplace_back(exact.intValue);
                }
            }
        } else if (ParseSourceAssignment(lines[lineNo - 1], name, expr)) {
            SourceExactValue exact;
            auto old = LookupSourceValue(scopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            bool hasExact = TryEvalSourceExactValue(expr, scopes, hint, exact, overflowStrategy);
            if (!hasExact && expr.find("spawn") != std::string::npos) {
                if (auto value = EvalSourceSpawnExprBlock(lines, lineNo, scopes); value.has_value()) {
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    hasExact = true;
                }
            }
            if (hasExact) {
                scopes.back()[name] = exact;
                if (name == variableName && IsSourceIntegerTypeHint(exact.typeHint)) {
                    values.emplace_back(exact.intValue);
                }
            }
        } else {
            std::string op;
            if (ParseSourceCompoundAssignment(lines[lineNo - 1], name, op, expr)) {
                auto value = EvalSourceCompoundAssignment(scopes, name, op, expr);
                if (value.has_value()) {
                    SourceExactValue exact;
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    scopes.back()[name] = exact;
                    if (name == variableName) {
                        values.emplace_back(value.value());
                    }
                }
            }
        }
        for (auto c : lines[lineNo - 1]) {
            if (c == '{') {
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                scopes.pop_back();
            }
        }
    }
}

bool SourceRangeHasZeroInitializer(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName)
{
    std::vector<SourceScope> emptyScopes(1);
    for (unsigned lineNo = begin; lineNo <= end && lineNo <= lines.size(); ++lineNo) {
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(lines[lineNo - 1], name, typeHint, expr) && name == variableName) {
            auto value = EvalSourceIntExpr(expr, emptyScopes);
            return value.has_value() && value.value() == 0;
        }
    }
    return false;
}

size_t CountSourceAssignments(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName)
{
    size_t count = 0;
    for (unsigned lineNo = begin; lineNo <= end && lineNo <= lines.size(); ++lineNo) {
        std::string name;
        std::string expr;
        if (ParseSourceAssignment(lines[lineNo - 1], name, expr) && name == variableName) {
            ++count;
            continue;
        }
        std::string op;
        if (ParseSourceCompoundAssignment(lines[lineNo - 1], name, op, expr) && name == variableName) {
            ++count;
        }
    }
    return count;
}

void DropCoveredSourceZeroInitializer(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName, std::vector<int64_t>& values)
{
    if (values.size() <= 1 || !SourceRangeHasZeroInitializer(lines, begin, end, variableName) ||
        CountSourceAssignments(lines, begin, end, variableName) < 2) {
        return;
    }
    values.erase(std::remove(values.begin(), values.end(), 0), values.end());
}

void InferSourceAssignedValuesFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    std::vector<int64_t> values;
    auto loop = FindInnermostSourceLoop(lines, query.line);
    if (loop.has_value()) {
        auto begin = SourceLineDeclaresVariable(lines[query.line - 1], query.variableName) ? query.line + 1 : loop->start + 1;
        auto end = loop->end > 0 ? loop->end - 1 : loop->end;
        if (begin <= end) {
            CollectSourceAssignedIntValues(lines, begin, end, query.variableName, values);
            DropCoveredSourceZeroInitializer(lines, begin, end, query.variableName, values);
        }
    } else {
        auto begin = query.line > 96 ? query.line - 96 : 1;
        CollectSourceAssignedIntValues(lines, begin, query.line, query.variableName, values);
        DropCoveredSourceZeroInitializer(lines, begin, query.line, query.variableName, values);
    }
    if (values.size() > 1) {
        SetSourceIntSetFallback(query, std::move(values));
    }
}

bool SourceLineAssignsVariableName(const std::string& line, const std::string& variableName)
{
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    if (ParseSourceDeclaration(line, name, typeHint, expr) && name == variableName) {
        return true;
    }
    if (ParseSourceAssignment(line, name, expr) && name == variableName) {
        return true;
    }
    std::string op;
    return ParseSourceCompoundAssignment(line, name, op, expr) && name == variableName;
}

bool SourceHasPriorLoopAssignment(
    const std::vector<std::string>& lines, unsigned line, const std::string& variableName)
{
    for (unsigned lineNo = 1; lineNo < line && lineNo <= lines.size(); ++lineNo) {
        if (SourceLineAssignsVariableName(lines[lineNo - 1], variableName) &&
            IsSourceLineInsideLoop(lines, lineNo)) {
            return true;
        }
    }
    return false;
}

bool SourceLineDeclaresMutableVariableName(const std::string& line, const std::string& variableName)
{
    auto trimmed = Trim(StripLineComment(line));
    auto pos = SkipSourceDeclarationModifiers(trimmed);
    if (trimmed.compare(pos, 3, "var") != 0 ||
        (pos + 3 < trimmed.size() && IsIdentifierChar(trimmed[pos + 3]))) {
        return false;
    }
    pos += 3;
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    auto begin = pos;
    while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
        ++pos;
    }
    return begin != pos && trimmed.substr(begin, pos - begin) == variableName;
}

bool SourceHasAssignmentAfterLine(const std::vector<std::string>& lines, unsigned line, const std::string& variableName)
{
    if (line >= lines.size()) {
        return false;
    }
    return CountSourceAssignments(lines, line + 1, static_cast<unsigned>(lines.size()), variableName) > 0;
}

bool ShouldSuppressMutableDeclarationFallback(
    const std::vector<std::string>& lines, const ContestQuery& query, unsigned lineNo, const std::string& name)
{
    if (lineNo != query.line || name != query.variableName || lineNo == 0 || lineNo > lines.size() ||
        !SourceLineDeclaresMutableVariableName(lines[lineNo - 1], query.variableName)) {
        return false;
    }
    return SourceHasAssignmentAfterLine(lines, lineNo, query.variableName);
}

bool SourceLineDeclaresVariableName(const std::string& line, const std::string& variableName)
{
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    return ParseSourceDeclaration(line, name, typeHint, expr) && name == variableName;
}

bool SourceLineDeclaresStaticVariableName(const std::string& line, const std::string& variableName)
{
    auto trimmed = Trim(StripLineComment(line));
    if (trimmed.find("static") == std::string::npos) {
        return false;
    }
    return SourceLineDeclaresVariableName(trimmed, variableName);
}

bool SourceHasTopLevelVariableDeclaration(const std::vector<std::string>& lines, const std::string& variableName)
{
    int braceDepth = 0;
    for (const auto& rawLine : lines) {
        auto line = StripLineComment(rawLine);
        if ((braceDepth == 0 && SourceLineDeclaresVariableName(line, variableName)) ||
            SourceLineDeclaresStaticVariableName(line, variableName)) {
            return true;
        }
        for (auto c : line) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}' && braceDepth > 0) {
                --braceDepth;
            }
        }
    }
    return false;
}

bool TryCollectSourceConstantIntWrites(
    const std::vector<std::string>& lines, const std::string& variableName, std::vector<int64_t>& values)
{
    std::vector<SourceScope> scopes(1);
    std::optional<int64_t> lastTargetValue;
    int braceDepth = 0;
    for (const auto& rawLine : lines) {
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(rawLine, name, typeHint, expr)) {
            SourceExactValue exact;
            const bool hasExact = TryEvalSourceExactValue(expr, scopes, typeHint, exact);
            if (hasExact) {
                scopes.back()[name] = exact;
                if (braceDepth == 0 || StripLineComment(rawLine).find("static") != std::string::npos) {
                    scopes.front()[name] = exact;
                }
            }
            if (name == variableName) {
                if (!hasExact || !IsSourceIntegerTypeHint(exact.typeHint)) {
                    return false;
                }
                values.emplace_back(exact.intValue);
                lastTargetValue = exact.intValue;
                scopes.front()[name] = exact;
            }
        } else if (ParseSourceAssignment(rawLine, name, expr)) {
            auto value = EvalSourceIntExpr(expr, scopes);
            if (value.has_value()) {
                SourceExactValue exact;
                exact.typeHint = ContestQueryTypeHint::INT64;
                exact.intValue = value.value();
                scopes.back()[name] = exact;
                if (braceDepth == 0 || StripLineComment(rawLine).find("static") != std::string::npos) {
                    scopes.front()[name] = exact;
                }
                if (name == variableName) {
                    values.emplace_back(value.value());
                    lastTargetValue = value.value();
                    scopes.front()[name] = exact;
                }
            } else if (name == variableName) {
                return false;
            }
        } else {
            std::string op;
            if (ParseSourceCompoundAssignment(rawLine, name, op, expr)) {
                if (name == variableName && LookupSourceValue(scopes, name) == nullptr && lastTargetValue.has_value()) {
                    SourceExactValue exact;
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = lastTargetValue.value();
                    scopes.back()[name] = exact;
                }
                auto value = EvalSourceCompoundAssignment(scopes, name, op, expr);
                if (value.has_value()) {
                    SourceExactValue exact;
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    scopes.back()[name] = exact;
                    if (braceDepth == 0 || StripLineComment(rawLine).find("static") != std::string::npos) {
                        scopes.front()[name] = exact;
                    }
                    if (name == variableName) {
                        values.emplace_back(value.value());
                        lastTargetValue = value.value();
                        scopes.front()[name] = exact;
                    }
                } else if (name == variableName) {
                    return false;
                }
            }
        }
        for (auto c : rawLine) {
            if (c == '{') {
                ++braceDepth;
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                if (braceDepth > 0) {
                    --braceDepth;
                }
                scopes.pop_back();
            }
        }
    }
    return !values.empty();
}

void InferSourceGlobalConstantFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL ||
        !SourceHasTopLevelVariableDeclaration(lines, query.variableName)) {
        return;
    }
    std::vector<int64_t> values;
    if (!TryCollectSourceConstantIntWrites(lines, query.variableName, values)) {
        return;
    }
    DropCoveredSourceZeroInitializer(lines, 1, static_cast<unsigned>(lines.size()), query.variableName, values);
    SetSourceIntSetFallback(query, std::move(values));
}

struct SourceEnumPayloadValue {
    std::string constructor;
    int64_t payload{0};
};

std::optional<SourceEnumPayloadValue> ParseSourceEnumPayloadExpr(
    const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    auto open = trimmed.find('(');
    auto close = trimmed.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return std::nullopt;
    }
    auto ctor = Trim(trimmed.substr(0, open));
    auto dot = ctor.find_last_of('.');
    if (dot != std::string::npos) {
        ctor = ctor.substr(dot + 1);
    }
    if (ctor.empty() || !std::all_of(ctor.begin(), ctor.end(), IsIdentifierChar)) {
        return std::nullopt;
    }
    auto payloadExpr = trimmed.substr(open + 1, close - open - 1);
    if (payloadExpr.find(',') != std::string::npos) {
        return std::nullopt;
    }
    auto payload = EvalSourceIntExpr(payloadExpr, scopes);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    return SourceEnumPayloadValue{ctor, payload.value()};
}

std::optional<std::string> ParseSourceMatchScrutinee(const std::string& line)
{
    auto trimmed = StripLineComment(line);
    auto matchPos = trimmed.find("match");
    if (matchPos == std::string::npos) {
        return std::nullopt;
    }
    auto open = trimmed.find('(', matchPos);
    if (open == std::string::npos) {
        return std::nullopt;
    }
    int depth = 0;
    for (size_t pos = open; pos < trimmed.size(); ++pos) {
        if (trimmed[pos] == '(') {
            ++depth;
        } else if (trimmed[pos] == ')') {
            --depth;
            if (depth == 0) {
                auto scrutinee = Trim(trimmed.substr(open + 1, pos - open - 1));
                return scrutinee.empty() ? std::nullopt : std::optional<std::string>(scrutinee);
            }
        }
    }
    return std::nullopt;
}

std::optional<SourceEnumPayloadValue> FindSourceEnumPayloadForScrutinee(
    const std::vector<std::string>& lines, unsigned beforeLine, const std::string& scrutinee)
{
    std::vector<SourceScope> scopes(1);
    std::optional<SourceEnumPayloadValue> payload;
    for (unsigned lineNo = 1; lineNo < beforeLine && lineNo <= lines.size(); ++lineNo) {
        auto line = lines[lineNo - 1];
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(line, name, typeHint, expr)) {
            SourceExactValue exact;
            if (TryEvalSourceExactValue(expr, scopes, typeHint, exact)) {
                scopes.back()[name] = exact;
            }
        }
        if (ParseSourceAssignment(line, name, expr) && name == scrutinee) {
            payload = ParseSourceEnumPayloadExpr(expr, scopes);
        }
        for (auto c : line) {
            if (c == '{') {
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                scopes.pop_back();
            }
        }
    }
    return payload;
}

std::optional<std::string> ParseSourceCasePayloadVariable(const std::string& casePrefix, const std::string& constructor)
{
    auto prefix = Trim(casePrefix);
    auto casePos = prefix.find("case");
    if (casePos == std::string::npos) {
        return std::nullopt;
    }
    auto open = prefix.find('(', casePos);
    auto close = prefix.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return std::nullopt;
    }
    auto ctor = Trim(prefix.substr(casePos + 4, open - (casePos + 4)));
    auto dot = ctor.find_last_of('.');
    if (dot != std::string::npos) {
        ctor = ctor.substr(dot + 1);
    }
    if (!constructor.empty() && ctor != constructor) {
        return std::nullopt;
    }
    auto variable = Trim(prefix.substr(open + 1, close - open - 1));
    if (variable.empty() || variable.find(',') != std::string::npos ||
        !std::all_of(variable.begin(), variable.end(), IsIdentifierChar)) {
        return std::nullopt;
    }
    return variable;
}

std::vector<SourceScope> BuildSourceMatchArmScopes(
    const std::string& casePrefix, const std::optional<SourceEnumPayloadValue>& enumPayload)
{
    std::vector<SourceScope> scopes(1);
    if (!enumPayload.has_value()) {
        return scopes;
    }
    auto variable = ParseSourceCasePayloadVariable(casePrefix, enumPayload->constructor);
    if (!variable.has_value()) {
        return scopes;
    }
    SourceExactValue exact;
    exact.typeHint = ContestQueryTypeHint::INT64;
    exact.intValue = enumPayload->payload;
    scopes.back()[variable.value()] = exact;
    return scopes;
}

std::string TrimSourceMatchArmExpr(std::string expr)
{
    expr = Trim(StripLineComment(expr));
    auto comma = expr.find(',');
    auto brace = expr.find('}');
    auto semi = expr.find(';');
    auto nextCase = expr.find(" case ");
    auto end = std::min({comma == std::string::npos ? expr.size() : comma,
        brace == std::string::npos ? expr.size() : brace, semi == std::string::npos ? expr.size() : semi,
        nextCase == std::string::npos ? expr.size() : nextCase});
    expr = Trim(expr.substr(0, end));
    if (expr.rfind("return ", 0) == 0) {
        expr = Trim(expr.substr(7));
    }
    return expr;
}

void CollectSourceMatchArmFollowupValues(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName, const std::vector<SourceScope>& armScopes, std::vector<int64_t>& values,
    bool collectBareValues = true)
{
    auto limit = std::min<unsigned>(end, begin + 6);
    for (unsigned lineNo = begin; lineNo <= limit && lineNo <= lines.size(); ++lineNo) {
        auto line = Trim(StripLineComment(lines[lineNo - 1]));
        if (line.empty() || line == "{" || line == "}") {
            continue;
        }
        if (line.find("=>") != std::string::npos || line.rfind("case ", 0) == 0) {
            return;
        }
        std::string assigned;
        std::string expr;
        if (ParseSourceAssignment(line, assigned, expr) && assigned == variableName) {
            if (auto value = EvalSourceIntExpr(expr, armScopes); value.has_value()) {
                values.emplace_back(value.value());
            }
            continue;
        }
        if (line.rfind("return ", 0) == 0) {
            line = Trim(line.substr(7));
        }
        line = TrimSourceMatchArmExpr(line);
        if (collectBareValues && EvalSourceIntExpr(line, armScopes).has_value()) {
            auto value = EvalSourceIntExpr(line, armScopes);
            values.emplace_back(value.value());
            continue;
        }
    }
}

void CollectSourceMatchArmIntValues(const std::vector<std::string>& lines, unsigned begin, unsigned end,
    const std::string& variableName, const std::optional<SourceEnumPayloadValue>& enumPayload,
    std::vector<int64_t>& values, bool collectBareValues = true)
{
    std::vector<SourceScope> emptyScopes(1);
    for (unsigned lineNo = begin; lineNo <= end && lineNo <= lines.size(); ++lineNo) {
        auto line = StripLineComment(lines[lineNo - 1]);
        std::string assigned;
        std::string assignedExpr;
        if (ParseSourceAssignment(line, assigned, assignedExpr) && assigned == variableName) {
            if (auto value = EvalSourceIntExpr(assignedExpr, emptyScopes); value.has_value()) {
                values.emplace_back(value.value());
            }
        }
        size_t pos = 0;
        while ((pos = line.find("=>", pos)) != std::string::npos) {
            auto armScopes = BuildSourceMatchArmScopes(line.substr(0, pos), enumPayload);
            auto expr = TrimSourceMatchArmExpr(line.substr(pos + 2));
            std::string inlineAssigned;
            std::string inlineAssignedExpr;
            if (ParseSourceAssignment(expr, inlineAssigned, inlineAssignedExpr) && inlineAssigned == variableName) {
                if (auto value = EvalSourceIntExpr(inlineAssignedExpr, armScopes); value.has_value()) {
                    values.emplace_back(value.value());
                }
            } else if (collectBareValues && EvalSourceIntExpr(expr, armScopes).has_value()) {
                auto value = EvalSourceIntExpr(expr, armScopes);
                values.emplace_back(value.value());
            } else {
                CollectSourceMatchArmFollowupValues(
                    lines, lineNo + 1, end, variableName, armScopes, values, collectBareValues);
            }
            pos += 2;
        }
    }
}

std::optional<unsigned> FindSourceBraceBlockEnd(const std::vector<std::string>& lines, unsigned startLine)
{
    int depth = 0;
    bool sawOpen = false;
    for (unsigned lineNo = startLine; lineNo <= lines.size(); ++lineNo) {
        auto line = StripLineComment(lines[lineNo - 1]);
        for (auto c : line) {
            if (c == '{') {
                ++depth;
                sawOpen = true;
            } else if (c == '}') {
                --depth;
                if (sawOpen && depth <= 0) {
                    return lineNo;
                }
            }
        }
        if (sawOpen && depth <= 0) {
            return lineNo;
        }
    }
    return std::nullopt;
}

size_t SkipSourceGenericParameterList(const std::string& line, size_t pos)
{
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '<') {
        return pos;
    }
    int depth = 0;
    for (; pos < line.size(); ++pos) {
        if (line[pos] == '<') {
            ++depth;
        } else if (line[pos] == '>') {
            --depth;
            if (depth == 0) {
                return pos + 1;
            }
        }
    }
    return pos;
}

void InferSourceMatchFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    auto collectFromMatch = [&lines, &query](
        unsigned matchLine, unsigned end, bool collectBareValues, std::vector<int64_t>& values) {
        auto scrutinee = ParseSourceMatchScrutinee(lines[matchLine - 1]);
        auto enumPayload = scrutinee.has_value() ? FindSourceEnumPayloadForScrutinee(lines, matchLine, scrutinee.value()) :
            std::optional<SourceEnumPayloadValue>{};
        CollectSourceMatchArmIntValues(lines, matchLine, end, query.variableName, enumPayload, values, collectBareValues);
    };
    auto line = StripLineComment(lines[query.line - 1]);
    std::vector<int64_t> values;
    unsigned matchLine = query.line;
    unsigned end = query.line;
    if (line.find("match") != std::string::npos && SourceLineAssignsVariableName(line, query.variableName)) {
        auto fallbackEnd = static_cast<unsigned>(std::min<size_t>(lines.size(), static_cast<size_t>(query.line) + 64));
        end = FindSourceBraceBlockEnd(lines, query.line).value_or(fallbackEnd);
        collectFromMatch(query.line, end, true, values);
    } else {
        auto begin = query.line > 128 ? query.line - 128 : 1;
        for (unsigned lineNo = begin; lineNo < query.line && lineNo <= lines.size(); ++lineNo) {
            auto candidate = StripLineComment(lines[lineNo - 1]);
            if (candidate.find("match") == std::string::npos) {
                continue;
            }
            auto fallbackEnd = static_cast<unsigned>(std::min<size_t>(lines.size(), static_cast<size_t>(lineNo) + 64));
            auto candidateEnd = FindSourceBraceBlockEnd(lines, lineNo).value_or(fallbackEnd);
            std::vector<int64_t> candidateValues;
            collectFromMatch(lineNo, candidateEnd, SourceLineAssignsVariableName(candidate, query.variableName), candidateValues);
            if (!candidateValues.empty()) {
                matchLine = lineNo;
                end = candidateEnd;
                values = std::move(candidateValues);
            }
        }
    }
    if (values.empty()) {
        auto limit = std::min<unsigned>(static_cast<unsigned>(lines.size()), query.line + 128);
        for (unsigned lineNo = query.line + 1; lineNo <= limit; ++lineNo) {
            auto candidate = StripLineComment(lines[lineNo - 1]);
            if (candidate.find("match") == std::string::npos) {
                continue;
            }
            auto fallbackEnd = static_cast<unsigned>(std::min<size_t>(lines.size(), static_cast<size_t>(lineNo) + 64));
            auto candidateEnd = FindSourceBraceBlockEnd(lines, lineNo).value_or(fallbackEnd);
            std::vector<int64_t> candidateValues;
            collectFromMatch(lineNo, candidateEnd, SourceLineAssignsVariableName(candidate, query.variableName), candidateValues);
            if (!candidateValues.empty()) {
                matchLine = lineNo;
                end = candidateEnd;
                values = std::move(candidateValues);
                break;
            }
        }
    }
    if (!values.empty()) {
        DropCoveredSourceZeroInitializer(lines, matchLine > 96 ? matchLine - 96 : 1, end, query.variableName, values);
        SetSourceIntSetFallback(query, std::move(values));
        query.preferSourceFallback = query.hasSourceFallback;
    }
}

std::vector<std::string> SplitSourceAddTerms(const std::string& expr)
{
    std::vector<std::string> terms;
    size_t begin = 0;
    for (size_t i = 0; i <= expr.size(); ++i) {
        if (i == expr.size() || expr[i] == '+') {
            terms.emplace_back(Trim(expr.substr(begin, i - begin)));
            begin = i + 1;
        }
    }
    return terms;
}

std::optional<std::vector<int64_t>> InferSourceWhileInductionValues(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& name, unsigned depth);
std::optional<std::vector<int64_t>> NormalizeSourceIntSet(std::vector<int64_t> values);

std::optional<std::pair<int64_t, int64_t>> InferSourceNameIntervalInLoop(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& name, unsigned beforeLine)
{
    if (auto forRange = ParseSourceForInRangeHeader(lines[loop.start - 1]);
        forRange.has_value() && forRange->variable == name) {
        return GetSourceForRangeBounds(forRange.value());
    }
    if (auto values = InferSourceWhileInductionValues(lines, loop, name, 0); values.has_value() && !values->empty()) {
        auto [minIt, maxIt] = std::minmax_element(values->begin(), values->end());
        return std::make_pair(*minIt, *maxIt);
    }
    std::vector<int64_t> values;
    std::vector<SourceScope> emptyScopes(1);
    for (unsigned lineNo = loop.start + 1; lineNo < beforeLine && lineNo <= loop.end && lineNo <= lines.size(); ++lineNo) {
        std::string assigned;
        std::string expr;
        if (!ParseSourceAssignment(lines[lineNo - 1], assigned, expr) || assigned != name) {
            continue;
        }
        if (auto value = EvalSourceIntExpr(expr, emptyScopes); value.has_value()) {
            values.emplace_back(value.value());
        }
    }
    if (values.empty()) {
        return std::nullopt;
    }
    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    return std::make_pair(*minIt, *maxIt);
}

std::optional<std::pair<int64_t, int64_t>> InferSourceDeltaInterval(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& accumulator,
    const std::string& expr, unsigned line)
{
    auto terms = SplitSourceAddTerms(expr);
    bool sawAccumulator = false;
    int64_t minDelta = 0;
    int64_t maxDelta = 0;
    std::vector<SourceScope> emptyScopes(1);
    for (auto term : terms) {
        if (term == accumulator) {
            sawAccumulator = true;
            continue;
        }
        std::optional<std::pair<int64_t, int64_t>> interval;
        if (auto literal = EvalSourceIntExpr(term, emptyScopes); literal.has_value()) {
            interval = std::make_pair(literal.value(), literal.value());
        } else {
            interval = InferSourceNameIntervalInLoop(lines, loop, term, line);
        }
        if (!interval.has_value()) {
            return std::nullopt;
        }
        minDelta += interval->first;
        maxDelta += interval->second;
    }
    if (!sawAccumulator || (minDelta == 0 && maxDelta == 0)) {
        return std::nullopt;
    }
    return std::make_pair(minDelta, maxDelta);
}

std::optional<int64_t> InferSourceAccumulatorInit(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& name)
{
    std::vector<SourceScope> emptyScopes(1);
    std::optional<int64_t> init;
    for (unsigned lineNo = 1; lineNo < loop.start; ++lineNo) {
        std::string declared;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(lines[lineNo - 1], declared, typeHint, expr) && declared == name) {
            init = EvalSourceIntExpr(expr, emptyScopes);
        }
        std::string assigned;
        if (ParseSourceAssignment(lines[lineNo - 1], assigned, expr) && assigned == name) {
            init = EvalSourceIntExpr(expr, emptyScopes);
        }
    }
    return init;
}

std::optional<std::vector<int64_t>> GetSourceForRangeIterationValues(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop)
{
    auto forRange = ParseSourceForInRangeHeader(lines[loop.start - 1]);
    if (!forRange.has_value()) {
        return std::nullopt;
    }
    auto bounds = GetSourceForRangeBounds(forRange.value());
    if (!bounds.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    for (int64_t value = bounds->first; value <= bounds->second; ++value) {
        values.emplace_back(value);
        if (values.size() > MAX_CONTEST_EXACT_VALUES) {
            return std::nullopt;
        }
    }
    return values;
}

std::optional<std::vector<int64_t>> InferSourceDeltaExactValues(const std::vector<std::string>& lines,
    const SourceLoopExtent& loop, const std::string& accumulator, const std::string& expr)
{
    auto iterations = GetSourceForRangeIterationValues(lines, loop);
    if (!iterations.has_value() || iterations->empty()) {
        return std::nullopt;
    }
    auto forRange = ParseSourceForInRangeHeader(lines[loop.start - 1]);
    if (!forRange.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> deltas(iterations->size(), 0);
    bool sawAccumulator = false;
    std::vector<SourceScope> emptyScopes(1);
    for (auto term : SplitSourceAddTerms(expr)) {
        term = StripSourceEnclosingParens(term);
        if (term == accumulator) {
            sawAccumulator = true;
            continue;
        }
        if (term == forRange->variable) {
            for (size_t i = 0; i < deltas.size(); ++i) {
                int64_t result = 0;
                if (__builtin_add_overflow(deltas[i], (*iterations)[i], &result)) {
                    return std::nullopt;
                }
                deltas[i] = result;
            }
            continue;
        }
        auto literal = EvalSourceIntExpr(term, emptyScopes);
        if (!literal.has_value()) {
            return std::nullopt;
        }
        for (auto& delta : deltas) {
            int64_t result = 0;
            if (__builtin_add_overflow(delta, literal.value(), &result)) {
                return std::nullopt;
            }
            delta = result;
        }
    }
    if (!sawAccumulator) {
        return std::nullopt;
    }
    return deltas;
}

std::optional<std::vector<int64_t>> InferSourceAccumulatorExactValues(const std::vector<std::string>& lines,
    const SourceLoopExtent& loop, const std::string& accumulator, int64_t init, unsigned queryLine)
{
    std::string assigned;
    std::string expr;
    std::optional<std::vector<int64_t>> deltas;
    if (queryLine <= lines.size() && ParseSourceAssignment(lines[queryLine - 1], assigned, expr) && assigned == accumulator) {
        deltas = InferSourceDeltaExactValues(lines, loop, accumulator, expr);
    }
    if (!deltas.has_value()) {
        for (unsigned lineNo = loop.start + 1; lineNo < loop.end && lineNo <= lines.size(); ++lineNo) {
            if (!ParseSourceAssignment(lines[lineNo - 1], assigned, expr) || assigned != accumulator) {
                continue;
            }
            deltas = InferSourceDeltaExactValues(lines, loop, accumulator, expr);
            if (deltas.has_value()) {
                break;
            }
        }
    }
    if (!deltas.has_value() || deltas->empty()) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    int64_t current = init;
    for (auto delta : deltas.value()) {
        values.emplace_back(current);
        int64_t next = 0;
        if (__builtin_add_overflow(current, delta, &next)) {
            return std::nullopt;
        }
        current = next;
        values.emplace_back(current);
        if (values.size() > MAX_CONTEST_EXACT_VALUES) {
            return std::nullopt;
        }
    }
    return values;
}

void InferSourceAccumulatorFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() ||
        query.typeHint == ContestQueryTypeHint::BOOL || query.typeHint == ContestQueryTypeHint::UINT8 ||
        query.typeHint == ContestQueryTypeHint::UINT16 || query.typeHint == ContestQueryTypeHint::UINT32 ||
        query.typeHint == ContestQueryTypeHint::UINT64) {
        return;
    }
    auto loop = FindInnermostSourceLoop(lines, query.line);
    if (!loop.has_value()) {
        return;
    }
    auto tripCount = ParseSourceTripCount(lines, loop.value());
    auto init = InferSourceAccumulatorInit(lines, loop.value(), query.variableName);
    if (!tripCount.has_value() || !init.has_value() || tripCount.value() <= 0) {
        return;
    }
    if (auto exactValues = InferSourceAccumulatorExactValues(
            lines, loop.value(), query.variableName, init.value(), query.line);
        exactValues.has_value() && !exactValues->empty() && exactValues->size() <= MAX_CONTEST_EXACT_VALUES) {
        query.accumulatorFallback = FormatSourceIntValues(std::move(exactValues.value()));
        query.hasAccumulatorFallback = true;
        return;
    }

    std::optional<std::pair<int64_t, int64_t>> delta;
    std::string assigned;
    std::string expr;
    if (ParseSourceAssignment(lines[query.line - 1], assigned, expr) && assigned == query.variableName) {
        delta = InferSourceDeltaInterval(lines, loop.value(), query.variableName, expr, query.line);
    }
    for (unsigned lineNo = loop->start + 1; lineNo < loop->end && lineNo <= lines.size(); ++lineNo) {
        if (!ParseSourceAssignment(lines[lineNo - 1], assigned, expr) || assigned != query.variableName) {
            continue;
        }
        auto current = InferSourceDeltaInterval(lines, loop.value(), query.variableName, expr, lineNo);
        if (!current.has_value()) {
            continue;
        }
        if (!delta.has_value()) {
            delta = current;
        } else {
            delta->first = std::min(delta->first, current->first);
            delta->second = std::max(delta->second, current->second);
        }
    }
    if (!delta.has_value()) {
        return;
    }
    std::vector<int64_t> candidates{init.value()};
    for (auto step : {delta->first, delta->second}) {
        for (auto count : {tripCount.value() - 1, tripCount.value()}) {
            int64_t value = 0;
            if (count >= 0 && !__builtin_mul_overflow(count, step, &value) &&
                !__builtin_add_overflow(init.value(), value, &value)) {
                candidates.emplace_back(value);
            }
        }
    }
    auto [lowerIt, upperIt] = std::minmax_element(candidates.begin(), candidates.end());
    auto lower = *lowerIt;
    auto upper = *upperIt;
    query.accumulatorFallback = "[" + std::to_string(lower) + ", " + std::to_string(upper) + ":1]";
    query.hasAccumulatorFallback = true;
}

// 将源码显式类型名转换为 contest query 的轻量类型提示。
ContestQueryTypeHint ParseContestQueryTypeHint(const std::string& typeName)
{
    if (typeName == "Bool") {
        return ContestQueryTypeHint::BOOL;
    }
    if (typeName == "Int8") {
        return ContestQueryTypeHint::INT8;
    }
    if (typeName == "Int16") {
        return ContestQueryTypeHint::INT16;
    }
    if (typeName == "Int32") {
        return ContestQueryTypeHint::INT32;
    }
    if (typeName == "Int64") {
        return ContestQueryTypeHint::INT64;
    }
    if (typeName == "UInt8") {
        return ContestQueryTypeHint::UINT8;
    }
    if (typeName == "UInt16") {
        return ContestQueryTypeHint::UINT16;
    }
    if (typeName == "UInt32") {
        return ContestQueryTypeHint::UINT32;
    }
    if (typeName == "UInt64") {
        return ContestQueryTypeHint::UINT64;
    }
    return ContestQueryTypeHint::UNKNOWN;
}

// 从源码声明行或函数参数列表中推断查询变量的显式类型。
ContestQueryTypeHint InferContestQueryTypeHintFromLine(const std::string& line, const std::string& variableName)
{
    size_t pos = 0;
    while ((pos = line.find(variableName, pos)) != std::string::npos) {
        auto before = pos == 0 ? '\0' : line[pos - 1];
        auto afterPos = pos + variableName.size();
        auto after = afterPos >= line.size() ? '\0' : line[afterPos];
        if (IsIdentifierChar(before) || IsIdentifierChar(after)) {
            pos = afterPos;
            continue;
        }
        while (afterPos < line.size() && std::isspace(static_cast<unsigned char>(line[afterPos]))) {
            ++afterPos;
        }
        if (afterPos >= line.size() || line[afterPos] != ':') {
            pos = afterPos;
            continue;
        }
        ++afterPos;
        while (afterPos < line.size() && std::isspace(static_cast<unsigned char>(line[afterPos]))) {
            ++afterPos;
        }
        auto typeBegin = afterPos;
        while (afterPos < line.size() && IsIdentifierChar(line[afterPos])) {
            ++afterPos;
        }
        return ParseContestQueryTypeHint(line.substr(typeBegin, afterPos - typeBegin));
    }
    return ContestQueryTypeHint::UNKNOWN;
}

ContestQueryTypeHint MergeSourceIntegerTypeHint(ContestQueryTypeHint lhs, ContestQueryTypeHint rhs)
{
    if (!IsSourceIntegerTypeHint(lhs)) {
        return rhs;
    }
    if (!IsSourceIntegerTypeHint(rhs)) {
        return lhs;
    }
    return lhs == rhs ? lhs : ContestQueryTypeHint::INT64;
}

ContestQueryTypeHint InferSourceIntegerTypeHintFromToken(const std::string& token)
{
    static const std::vector<std::pair<std::string, ContestQueryTypeHint>> typePrefixes{
        {"Int8", ContestQueryTypeHint::INT8}, {"Int16", ContestQueryTypeHint::INT16},
        {"Int32", ContestQueryTypeHint::INT32}, {"Int64", ContestQueryTypeHint::INT64},
        {"UInt8", ContestQueryTypeHint::UINT8}, {"UInt16", ContestQueryTypeHint::UINT16},
        {"UInt32", ContestQueryTypeHint::UINT32}, {"UInt64", ContestQueryTypeHint::UINT64},
    };
    for (const auto& [name, hint] : typePrefixes) {
        if (token == name || token.rfind(name + ".", 0) == 0 || token.rfind(name + "(", 0) == 0) {
            return hint;
        }
    }
    return ContestQueryTypeHint::UNKNOWN;
}

ContestQueryTypeHint InferSourceIntegerTypeHintFromLiteralSuffix(const std::string& token)
{
    if (token.size() < 2) {
        return ContestQueryTypeHint::UNKNOWN;
    }
    auto lower = token;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::vector<std::pair<std::string, ContestQueryTypeHint>> suffixes{
        {"i8", ContestQueryTypeHint::INT8}, {"i16", ContestQueryTypeHint::INT16},
        {"i32", ContestQueryTypeHint::INT32}, {"i64", ContestQueryTypeHint::INT64},
        {"u8", ContestQueryTypeHint::UINT8}, {"u16", ContestQueryTypeHint::UINT16},
        {"u32", ContestQueryTypeHint::UINT32}, {"u64", ContestQueryTypeHint::UINT64},
    };
    for (const auto& [suffix, hint] : suffixes) {
        if (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return hint;
        }
    }
    return ContestQueryTypeHint::UNKNOWN;
}

ContestQueryTypeHint InferSourceIntegerTypeHintFromExpr(
    const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    ContestQueryTypeHint hint = InferSourceIntegerTypeHintFromToken(trimmed);
    if (IsSourceIntegerTypeHint(hint)) {
        return hint;
    }
    std::string token;
    auto flushToken = [&]() {
        if (token.empty()) {
            return;
        }
        hint = MergeSourceIntegerTypeHint(hint, InferSourceIntegerTypeHintFromToken(token));
        hint = MergeSourceIntegerTypeHint(hint, InferSourceIntegerTypeHintFromLiteralSuffix(token));
        if (auto value = LookupSourceValue(scopes, token);
            value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
            hint = MergeSourceIntegerTypeHint(hint, value->typeHint);
        }
        token.clear();
    };
    for (auto c : trimmed) {
        if (IsIdentifierChar(c) || c == '.') {
            token.push_back(c);
            continue;
        }
        flushToken();
    }
    flushToken();
    return hint;
}

// 从内向外查找源码常量环境中已经证明的简单值。
const SourceExactValue* LookupSourceValue(const std::vector<SourceScope>& scopes, const std::string& name)
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto value = it->find(name);
        if (value != it->end()) {
            return &value->second;
        }
    }
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < name.size()) {
        auto shortName = Trim(name.substr(dot + 1));
        if (!shortName.empty() && shortName != name) {
            return LookupSourceValue(scopes, shortName);
        }
    }
    return nullptr;
}

bool TryEvalSourceExactValue(const std::string& expr, const std::vector<SourceScope>& scopes,
    ContestQueryTypeHint preferredHint, SourceExactValue& value, SourceOverflowStrategy overflowStrategy)
{
    if (preferredHint != ContestQueryTypeHint::BOOL) {
        auto effectiveHint = IsSourceIntegerTypeHint(preferredHint) ? preferredHint :
            InferSourceIntegerTypeHintFromExpr(expr, scopes);
        auto intValue = overflowStrategy == SourceOverflowStrategy::NA
            ? EvalSourceIntExpr(expr, scopes)
            : EvalSourceIntExprWithOverflow(expr, scopes, effectiveHint, overflowStrategy);
        if (intValue.has_value()) {
            intValue = NormalizeSourceIntegerValue(*intValue, effectiveHint, overflowStrategy);
        }
        if (intValue.has_value()) {
            value.typeHint = IsSourceIntegerTypeHint(effectiveHint) ? effectiveHint : ContestQueryTypeHint::INT64;
            value.intValue = intValue.value();
            return true;
        }
    }
    if (preferredHint == ContestQueryTypeHint::BOOL || preferredHint == ContestQueryTypeHint::UNKNOWN) {
        if (auto boolValue = EvalSourceBoolExpr(expr, scopes); boolValue.has_value()) {
            value.typeHint = ContestQueryTypeHint::BOOL;
            value.boolValue = boolValue.value();
            return true;
        }
    }
    return false;
}

// 去掉源码行尾注释，避免简单表达式解析被注释内容干扰。
std::string StripLineComment(const std::string& line)
{
    auto comment = line.find("//");
    return comment == std::string::npos ? line : line.substr(0, comment);
}

// 从源码声明行中提取 let/var 名称、显式类型和初始化表达式。
bool ParseSourceDeclaration(
    const std::string& line, std::string& name, ContestQueryTypeHint& typeHint, std::string& expr)
{
    auto trimmed = Trim(StripLineComment(line));
    const std::string letPrefix = "let";
    const std::string varPrefix = "var";
    size_t pos = SkipSourceDeclarationModifiers(trimmed);
    if (trimmed.compare(pos, letPrefix.size(), letPrefix) == 0 &&
        (pos + letPrefix.size() == trimmed.size() || !IsIdentifierChar(trimmed[pos + letPrefix.size()]))) {
        pos += letPrefix.size();
    } else if (trimmed.compare(pos, varPrefix.size(), varPrefix) == 0 &&
        (pos + varPrefix.size() == trimmed.size() || !IsIdentifierChar(trimmed[pos + varPrefix.size()]))) {
        pos += varPrefix.size();
    } else {
        size_t cStylePos = pos;
        bool isSigned = false;
        bool isUnsigned = false;
        int longCount = 0;
        bool sawType = false;
        std::optional<ContestQueryTypeHint> cStyleHint;
        while (cStylePos < trimmed.size()) {
            while (cStylePos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cStylePos]))) {
                ++cStylePos;
            }
            auto tokenBegin = cStylePos;
            while (cStylePos < trimmed.size() && IsIdentifierChar(trimmed[cStylePos])) {
                ++cStylePos;
            }
            auto token = trimmed.substr(tokenBegin, cStylePos - tokenBegin);
            if (token == "signed") {
                isSigned = true;
                sawType = true;
                continue;
            }
            if (token == "unsigned") {
                isUnsigned = true;
                sawType = true;
                continue;
            }
            if (token == "long") {
                ++longCount;
                sawType = true;
                continue;
            }
            if (token == "short" || token == "int") {
                sawType = true;
                continue;
            }
            if (token == "int8_t" || token == "int16_t" || token == "int32_t" || token == "int64_t" ||
                token == "uint8_t" || token == "uint16_t" || token == "uint32_t" || token == "uint64_t") {
                sawType = true;
                cStyleHint = token == "int8_t" ? ContestQueryTypeHint::INT8 :
                    token == "int16_t" ? ContestQueryTypeHint::INT16 :
                    token == "int32_t" ? ContestQueryTypeHint::INT32 :
                    token == "int64_t" ? ContestQueryTypeHint::INT64 :
                    token == "uint8_t" ? ContestQueryTypeHint::UINT8 :
                    token == "uint16_t" ? ContestQueryTypeHint::UINT16 :
                    token == "uint32_t" ? ContestQueryTypeHint::UINT32 :
                    ContestQueryTypeHint::UINT64;
                continue;
            }
            cStylePos = tokenBegin;
            break;
        }
        while (cStylePos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cStylePos]))) {
            ++cStylePos;
        }
        if (!sawType || cStylePos >= trimmed.size() || !IsIdentifierChar(trimmed[cStylePos]) ||
            std::isdigit(static_cast<unsigned char>(trimmed[cStylePos])) || (isSigned && isUnsigned)) {
            return false;
        }
        pos = cStylePos;
        if (cStyleHint.has_value()) {
            typeHint = cStyleHint.value();
        } else if (isUnsigned) {
            typeHint = longCount >= 2 ? ContestQueryTypeHint::UINT64 : ContestQueryTypeHint::UINT32;
        } else {
            typeHint = longCount >= 2 ? ContestQueryTypeHint::INT64 : ContestQueryTypeHint::INT32;
        }
        auto nameBegin = pos;
        while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
            ++pos;
        }
        name = trimmed.substr(nameBegin, pos - nameBegin);
        while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
            ++pos;
        }
        if (pos >= trimmed.size() || trimmed[pos] != '=' || (pos + 1 < trimmed.size() && trimmed[pos + 1] == '=')) {
            return false;
        }
        expr = Trim(trimmed.substr(pos + 1));
        if (!expr.empty() && expr.back() == ';') {
            expr = Trim(expr.substr(0, expr.size() - 1));
        }
        return !expr.empty();
    }
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    auto nameBegin = pos;
    while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
        ++pos;
    }
    if (nameBegin == pos) {
        return false;
    }
    name = trimmed.substr(nameBegin, pos - nameBegin);
    typeHint = ContestQueryTypeHint::UNKNOWN;
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos < trimmed.size() && trimmed[pos] == ':') {
        ++pos;
        while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
            ++pos;
        }
        auto typeBegin = pos;
        while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
            ++pos;
        }
        typeHint = ParseContestQueryTypeHint(trimmed.substr(typeBegin, pos - typeBegin));
        int typeDepth = 0;
        while (pos < trimmed.size()) {
            if (trimmed[pos] == '(' || trimmed[pos] == '<' || trimmed[pos] == '[') {
                ++typeDepth;
            } else if ((trimmed[pos] == ')' || trimmed[pos] == '>' || trimmed[pos] == ']') && typeDepth > 0) {
                --typeDepth;
            } else if (trimmed[pos] == '=' && typeDepth == 0) {
                break;
            }
            ++pos;
        }
    }
    if (pos >= trimmed.size() || trimmed[pos] != '=') {
        return false;
    }
    if (pos + 1 < trimmed.size() && trimmed[pos + 1] == '=') {
        return false;
    }
    expr = Trim(trimmed.substr(pos + 1));
    return !expr.empty();
}

bool IsSourceDigitForBase(char c, int base)
{
    if (c == '_') {
        return true;
    }
    if (base == 2) {
        return c == '0' || c == '1';
    }
    if (base == 16) {
        return std::isxdigit(static_cast<unsigned char>(c));
    }
    return std::isdigit(static_cast<unsigned char>(c));
}

std::optional<int64_t> ParseSourceIntLiteral(const std::string& text)
{
    auto trimmed = Trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    size_t pos = 0;
    if (trimmed[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= trimmed.size()) {
        return std::nullopt;
    }
    int base = 10;
    if (pos + 1 < trimmed.size() && trimmed[pos] == '0' && (trimmed[pos + 1] == 'x' || trimmed[pos + 1] == 'X')) {
        base = 16;
        pos += 2;
    } else if (pos + 1 < trimmed.size() && trimmed[pos] == '0' && (trimmed[pos + 1] == 'b' || trimmed[pos + 1] == 'B')) {
        base = 2;
        pos += 2;
    }
    std::string digits;
    while (pos < trimmed.size() && IsSourceDigitForBase(trimmed[pos], base)) {
        if (trimmed[pos] != '_') {
            digits.push_back(trimmed[pos]);
        }
        ++pos;
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
        ++pos;
    }
    if (pos != trimmed.size()) {
        return std::nullopt;
    }
    try {
        auto parsed = std::stoll(digits, nullptr, base);
        return negative ? -parsed : parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int64_t> EvalSourceIntExpr(const std::string& expr, const std::vector<SourceScope>& scopes);

bool HasSourceEnclosingParens(const std::string& expr)
{
    auto trimmed = Trim(expr);
    if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')') {
        return false;
    }
    int depth = 0;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == '(') {
            ++depth;
        } else if (trimmed[i] == ')') {
            --depth;
            if (depth == 0 && i + 1 != trimmed.size()) {
                return false;
            }
        }
        if (depth < 0) {
            return false;
        }
    }
    return depth == 0;
}

std::string StripSourceEnclosingParens(std::string expr)
{
    expr = Trim(expr);
    while (HasSourceEnclosingParens(expr)) {
        expr = Trim(expr.substr(1, expr.size() - 2));
    }
    return expr;
}

bool IsSourceUnaryOperatorPosition(const std::string& expr, size_t pos)
{
    if (pos == 0) {
        return true;
    }
    auto prev = expr[pos - 1];
    return prev == '(' || prev == '+' || prev == '-' || prev == '*' || prev == '/' || prev == '%' || prev == '&' ||
        prev == '|' || prev == '^' || prev == '<' || prev == '>' || prev == '=' || prev == '!';
}

std::optional<size_t> FindSourceBinaryToken(const std::string& expr, const std::string& token)
{
    if (token == "**") {
        int depth = 0;
        for (size_t pos = 0; pos < expr.size(); ++pos) {
            if (expr[pos] == '(') {
                ++depth;
                continue;
            }
            if (expr[pos] == ')') {
                --depth;
                continue;
            }
            if (depth == 0 && pos + token.size() <= expr.size() && expr.compare(pos, token.size(), token) == 0) {
                return pos;
            }
        }
        return std::nullopt;
    }
    int depth = 0;
    for (size_t i = expr.size(); i > 0; --i) {
        auto pos = i - 1;
        if (expr[pos] == ')') {
            ++depth;
            continue;
        }
        if (expr[pos] == '(') {
            --depth;
            continue;
        }
        if (depth != 0 || pos + token.size() > expr.size() || expr.compare(pos, token.size(), token) != 0) {
            continue;
        }
        if (token == "*" &&
            ((pos > 0 && expr[pos - 1] == '*') || (pos + 1 < expr.size() && expr[pos + 1] == '*'))) {
            continue;
        }
        if ((token == "+" || token == "-") && IsSourceUnaryOperatorPosition(expr, pos)) {
            continue;
        }
        return pos;
    }
    return std::nullopt;
}

std::optional<int64_t> EvalSourceBinaryIntOp(int64_t lhs, int64_t rhs, const std::string& token)
{
    int64_t result = 0;
    if (token == "+") {
        if (__builtin_add_overflow(lhs, rhs, &result)) {
            return std::nullopt;
        }
        return result;
    }
    if (token == "-") {
        if (__builtin_sub_overflow(lhs, rhs, &result)) {
            return std::nullopt;
        }
        return result;
    }
    if (token == "*") {
        if (__builtin_mul_overflow(lhs, rhs, &result)) {
            return std::nullopt;
        }
        return result;
    }
    if (token == "/") {
        if (rhs == 0 || (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)) {
            return std::nullopt;
        }
        return lhs / rhs;
    }
    if (token == "%") {
        if (rhs == 0 || (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)) {
            return std::nullopt;
        }
        return lhs % rhs;
    }
    if (token == "**") {
        if (rhs < 0) {
            return std::nullopt;
        }
        int64_t value = 1;
        int64_t base = lhs;
        auto exponent = static_cast<uint64_t>(rhs);
        while (exponent != 0) {
            if ((exponent & 1U) != 0 && __builtin_mul_overflow(value, base, &value)) {
                return std::nullopt;
            }
            exponent >>= 1U;
            if (exponent != 0 && __builtin_mul_overflow(base, base, &base)) {
                return std::nullopt;
            }
        }
        return value;
    }
    if (token == "&") {
        return lhs & rhs;
    }
    if (token == "|") {
        return lhs | rhs;
    }
    if (token == "^") {
        return lhs ^ rhs;
    }
    if (token == "<<") {
        if (rhs < 0 || rhs >= 64) {
            return std::nullopt;
        }
        return SInt{IntWidth::I64, static_cast<uint64_t>(lhs)}.Shl(static_cast<unsigned>(rhs)).SVal();
    }
    if (token == ">>") {
        if (rhs < 0 || rhs >= 64) {
            return std::nullopt;
        }
        return SInt{IntWidth::I64, static_cast<uint64_t>(lhs)}.Ashr(static_cast<unsigned>(rhs)).SVal();
    }
    return std::nullopt;
}

std::optional<int64_t> EvalSourceArithmeticWithOverflow(
    int64_t lhs, int64_t rhs, const std::string& token, ContestQueryTypeHint hint,
    SourceOverflowStrategy overflowStrategy)
{
    auto width = GetSourceIntegerWidth(hint);
    if (!width.has_value() || !IsSourceIntegerTypeHint(hint) ||
        overflowStrategy == SourceOverflowStrategy::NA) {
        return EvalSourceBinaryIntOp(lhs, rhs, token);
    }
    auto isUnsigned = IsSourceUnsignedIntegerTypeHint(hint);
    if (token != "+" && token != "-" && token != "*" && token != "**") {
        auto value = EvalSourceBinaryIntOp(lhs, rhs, token);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return NormalizeSourceIntegerValue(*value, hint, overflowStrategy);
    }
    if (token == "**" && rhs < 0) {
        return std::nullopt;
    }
    if (overflowStrategy == SourceOverflowStrategy::WRAPPING) {
        auto mask = GetSourceIntegerMask(*width);
        uint64_t result = 0;
        if (token == "+") {
            result = (static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs)) & mask;
        } else if (token == "-") {
            result = (static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs)) & mask;
        } else if (token == "*") {
            result = static_cast<uint64_t>(
                static_cast<unsigned __int128>(static_cast<uint64_t>(lhs)) *
                static_cast<unsigned __int128>(static_cast<uint64_t>(rhs))) & mask;
        } else {
            result = 1ULL & mask;
            auto base = static_cast<uint64_t>(lhs) & mask;
            auto exponent = static_cast<uint64_t>(rhs);
            while (exponent != 0) {
                if ((exponent & 1U) != 0) {
                    result = static_cast<uint64_t>(
                        static_cast<unsigned __int128>(result) * static_cast<unsigned __int128>(base)) & mask;
                }
                exponent >>= 1U;
                if (exponent != 0) {
                    base = static_cast<uint64_t>(
                        static_cast<unsigned __int128>(base) * static_cast<unsigned __int128>(base)) & mask;
                }
            }
        }
        if (isUnsigned) {
            return static_cast<int64_t>(result);
        }
        return SignExtendSourceInteger(result, *width);
    }

    if (isUnsigned) {
        auto [lower, upper] = GetSourceUnsignedBounds(*width);
        unsigned __int128 lhsValue = lhs < 0 ? 0 : static_cast<unsigned __int128>(static_cast<uint64_t>(lhs));
        unsigned __int128 rhsValue = rhs < 0 ? 0 : static_cast<unsigned __int128>(static_cast<uint64_t>(rhs));
        unsigned __int128 result = 0;
        if (token == "+") {
            result = lhsValue + rhsValue;
        } else if (token == "-") {
            if (lhsValue < rhsValue) {
                if (overflowStrategy == SourceOverflowStrategy::SATURATING) {
                    return static_cast<int64_t>(lower);
                }
                return std::nullopt;
            }
            result = lhsValue - rhsValue;
        } else if (token == "*") {
            result = lhsValue * rhsValue;
        } else {
            result = 1;
            auto base = lhsValue;
            auto exponent = static_cast<uint64_t>(rhs);
            while (exponent != 0) {
                if ((exponent & 1U) != 0) {
                    result *= base;
                    if (result > upper) {
                        break;
                    }
                }
                exponent >>= 1U;
                if (exponent != 0) {
                    base *= base;
                    if (base > upper) {
                        base = upper + 1;
                    }
                }
            }
        }
        if (result > upper) {
            if (overflowStrategy == SourceOverflowStrategy::SATURATING) {
                return static_cast<int64_t>(upper);
            }
            return std::nullopt;
        }
        return static_cast<int64_t>(result);
    }

    auto [lower, upper] = GetSourceSignedBounds(*width);
    __int128 lhsValue = static_cast<__int128>(lhs);
    __int128 rhsValue = static_cast<__int128>(rhs);
    __int128 result = 0;
    bool overflow = false;
    if (token == "+") {
        result = lhsValue + rhsValue;
    } else if (token == "-") {
        result = lhsValue - rhsValue;
    } else if (token == "*") {
        result = lhsValue * rhsValue;
    } else {
        result = 1;
        auto base = lhsValue;
        auto exponent = static_cast<uint64_t>(rhs);
        while (exponent != 0) {
            if ((exponent & 1U) != 0) {
                result *= base;
                if (result < lower || result > upper) {
                    overflow = true;
                    break;
                }
            }
            exponent >>= 1U;
            if (exponent != 0) {
                base *= base;
                if (base < lower || base > upper) {
                    base = base < 0 ? lower - 1 : upper + 1;
                }
            }
        }
    }
    overflow = overflow || result < lower || result > upper;
    if (overflow) {
        if (overflowStrategy == SourceOverflowStrategy::SATURATING) {
            if (token == "**" && lhs < 0 && (static_cast<uint64_t>(rhs) % 2U) == 1U) {
                return static_cast<int64_t>(lower);
            }
            return result < lower ? static_cast<int64_t>(lower) : static_cast<int64_t>(upper);
        }
        return std::nullopt;
    }
    return static_cast<int64_t>(result);
}

std::optional<int64_t> ParseSourceNamedIntConstant(std::string text)
{
    text = Trim(text);
    const std::unordered_map<std::string, int64_t> constants{
        {"Int8.Min", std::numeric_limits<int8_t>::min()},
        {"Int8.Max", std::numeric_limits<int8_t>::max()},
        {"Int16.Min", std::numeric_limits<int16_t>::min()},
        {"Int16.Max", std::numeric_limits<int16_t>::max()},
        {"Int32.Min", std::numeric_limits<int32_t>::min()},
        {"Int32.Max", std::numeric_limits<int32_t>::max()},
        {"Int64.Min", std::numeric_limits<int64_t>::min()},
        {"Int64.Max", std::numeric_limits<int64_t>::max()},
        {"UInt8.Min", 0},
        {"UInt8.Max", std::numeric_limits<uint8_t>::max()},
        {"UInt16.Min", 0},
        {"UInt16.Max", std::numeric_limits<uint16_t>::max()},
        {"UInt32.Min", 0},
        {"UInt32.Max", std::numeric_limits<uint32_t>::max()},
    };
    auto it = constants.find(text);
    if (it != constants.end()) {
        return it->second;
    }
    auto valueSuffix = text.find(".MaxValue");
    if (valueSuffix != std::string::npos) {
        text.replace(valueSuffix, 9, ".Max");
        if (auto retry = constants.find(text); retry != constants.end()) {
            return retry->second;
        }
    }
    valueSuffix = text.find(".MinValue");
    if (valueSuffix != std::string::npos) {
        text.replace(valueSuffix, 9, ".Min");
        if (auto retry = constants.find(text); retry != constants.end()) {
            return retry->second;
        }
    }
    return std::nullopt;
}

// Conservative evaluator for simple Int64 source constants used by contest fallback.
std::optional<int64_t> EvalSourceIntExpr(const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    if (trimmed.rfind("try ", 0) == 0) {
        return EvalSourceIntExpr(trimmed.substr(4), scopes);
    }
    for (const auto& suffix : {std::string(".get()"), std::string(".getOrThrow()")}) {
        if (trimmed.size() > suffix.size() && trimmed.compare(trimmed.size() - suffix.size(), suffix.size(), suffix) == 0) {
            auto base = Trim(trimmed.substr(0, trimmed.size() - suffix.size()));
            if (auto value = LookupSourceValue(scopes, base);
                value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
                return value->intValue;
            }
        }
    }
    const std::vector<std::string> integerCasts{
        "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64"};
    for (const auto& typeName : integerCasts) {
        auto prefix = typeName + "(";
        if (trimmed.size() > prefix.size() + 1 && trimmed.rfind(prefix, 0) == 0 && trimmed.back() == ')') {
            return EvalSourceIntExpr(trimmed.substr(prefix.size(), trimmed.size() - prefix.size() - 1), scopes);
        }
    }
    const std::vector<std::vector<std::string>> precedenceGroups{{"|"}, {"^"}, {"&"}, {"<<", ">>"}, {"+", "-"},
        {"*", "/", "%"}, {"**"}};
    for (const auto& group : precedenceGroups) {
        for (const auto& token : group) {
            auto pos = FindSourceBinaryToken(trimmed, token);
            if (!pos.has_value()) {
                continue;
            }
            auto lhs = EvalSourceIntExpr(trimmed.substr(0, pos.value()), scopes);
            auto rhs = EvalSourceIntExpr(trimmed.substr(pos.value() + token.size()), scopes);
            if (!lhs.has_value() || !rhs.has_value()) {
                return std::nullopt;
            }
            return EvalSourceBinaryIntOp(lhs.value(), rhs.value(), token);
        }
    }
    if (auto literal = ParseSourceIntLiteral(trimmed); literal.has_value()) {
        return literal;
    }
    if (auto namedConstant = ParseSourceNamedIntConstant(trimmed); namedConstant.has_value()) {
        return namedConstant;
    }
    if (auto value = LookupSourceValue(scopes, trimmed);
        value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
        return value->intValue;
    }
    return std::nullopt;
}

std::optional<int64_t> EvalSourceIntExprWithOverflow(const std::string& expr,
    const std::vector<SourceScope>& scopes, ContestQueryTypeHint preferredHint,
    SourceOverflowStrategy overflowStrategy)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    if (overflowStrategy == SourceOverflowStrategy::NA || !IsSourceIntegerTypeHint(preferredHint)) {
        return EvalSourceIntExpr(trimmed, scopes);
    }
    if (trimmed.rfind("try ", 0) == 0) {
        return EvalSourceIntExprWithOverflow(trimmed.substr(4), scopes, preferredHint, overflowStrategy);
    }
    for (const auto& suffix : {std::string(".get()"), std::string(".getOrThrow()")}) {
        if (trimmed.size() > suffix.size() && trimmed.compare(trimmed.size() - suffix.size(), suffix.size(), suffix) == 0) {
            auto base = Trim(trimmed.substr(0, trimmed.size() - suffix.size()));
            if (auto value = LookupSourceValue(scopes, base);
                value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
                return NormalizeSourceIntegerValue(value->intValue, preferredHint, overflowStrategy);
            }
        }
    }
    const std::vector<std::vector<std::string>> precedenceGroups{{"|"}, {"^"}, {"&"}, {"<<", ">>"}, {"+", "-"},
        {"*", "/", "%"}, {"**"}};
    for (const auto& group : precedenceGroups) {
        for (const auto& token : group) {
            auto pos = FindSourceBinaryToken(trimmed, token);
            if (!pos.has_value()) {
                continue;
            }
            auto lhs = EvalSourceIntExprWithOverflow(
                trimmed.substr(0, pos.value()), scopes, preferredHint, overflowStrategy);
            auto rhs = EvalSourceIntExprWithOverflow(
                trimmed.substr(pos.value() + token.size()), scopes, preferredHint, overflowStrategy);
            if (!lhs.has_value() || !rhs.has_value()) {
                return std::nullopt;
            }
            return EvalSourceArithmeticWithOverflow(lhs.value(), rhs.value(), token, preferredHint, overflowStrategy);
        }
    }
    if (auto literal = ParseSourceIntLiteral(trimmed); literal.has_value()) {
        return NormalizeSourceIntegerValue(*literal, preferredHint, overflowStrategy);
    }
    if (auto namedConstant = ParseSourceNamedIntConstant(trimmed); namedConstant.has_value()) {
        return NormalizeSourceIntegerValue(*namedConstant, preferredHint, overflowStrategy);
    }
    if (auto value = LookupSourceValue(scopes, trimmed);
        value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
        return NormalizeSourceIntegerValue(value->intValue, preferredHint, overflowStrategy);
    }
    return std::nullopt;
}

std::optional<bool> EvalSourceBoolExpr(const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    if (trimmed == "true") {
        return true;
    }
    if (trimmed == "false") {
        return false;
    }
    if (!trimmed.empty() && trimmed.front() == '!') {
        auto inner = EvalSourceBoolExpr(trimmed.substr(1), scopes);
        return inner.has_value() ? std::optional<bool>{!inner.value()} : std::nullopt;
    }
    if (auto pos = FindSourceBinaryToken(trimmed, "||"); pos.has_value()) {
        auto lhs = EvalSourceBoolExpr(trimmed.substr(0, pos.value()), scopes);
        if (lhs.has_value() && lhs.value()) {
            return true;
        }
        auto rhs = EvalSourceBoolExpr(trimmed.substr(pos.value() + 2), scopes);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        return lhs.value() || rhs.value();
    }
    if (auto pos = FindSourceBinaryToken(trimmed, "&&"); pos.has_value()) {
        auto lhs = EvalSourceBoolExpr(trimmed.substr(0, pos.value()), scopes);
        if (lhs.has_value() && !lhs.value()) {
            return false;
        }
        auto rhs = EvalSourceBoolExpr(trimmed.substr(pos.value() + 2), scopes);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        return lhs.value() && rhs.value();
    }
    if (auto value = LookupSourceValue(scopes, trimmed);
        value != nullptr && value->typeHint == ContestQueryTypeHint::BOOL) {
        return value->boolValue;
    }
    const std::vector<std::pair<std::string, RelationalOperation>> relations{
        {">=", RelationalOperation::GE}, {"<=", RelationalOperation::LE}, {"==", RelationalOperation::EQ},
        {"!=", RelationalOperation::NE}, {">", RelationalOperation::GT}, {"<", RelationalOperation::LT}};
    for (const auto& [token, rel] : relations) {
        auto pos = trimmed.find(token);
        if (pos == std::string::npos) {
            continue;
        }
        auto lhs = EvalSourceIntExpr(trimmed.substr(0, pos), scopes);
        auto rhs = EvalSourceIntExpr(trimmed.substr(pos + token.size()), scopes);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        switch (rel) {
            case RelationalOperation::GE:
                return lhs.value() >= rhs.value();
            case RelationalOperation::LE:
                return lhs.value() <= rhs.value();
            case RelationalOperation::EQ:
                return lhs.value() == rhs.value();
            case RelationalOperation::NE:
                return lhs.value() != rhs.value();
            case RelationalOperation::GT:
                return lhs.value() > rhs.value();
            case RelationalOperation::LT:
                return lhs.value() < rhs.value();
        }
    }
    return std::nullopt;
}

struct SourceVariableComparison {
    std::string lhs;
    std::string rhs;
    RelationalOperation relation;
};

struct SourcePairLoopInfo {
    SourceLoopExtent loop;
    SourceVariableComparison comparison;
    int64_t lhsStep{0};
    int64_t rhsStep{0};
    std::vector<int64_t> lhsInits;
    std::vector<int64_t> rhsInits;
};

struct SourcePairLoopValues {
    std::vector<int64_t> lhsHeaderValues;
    std::vector<int64_t> rhsHeaderValues;
    std::vector<int64_t> lhsBodyValues;
    std::vector<int64_t> rhsBodyValues;
    std::vector<int64_t> lhsPostBodyValues;
    std::vector<int64_t> rhsPostBodyValues;
    std::vector<int64_t> lhsExitValues;
    std::vector<int64_t> rhsExitValues;
};

bool IsSourceIdentifierName(const std::string& name)
{
    auto trimmed = Trim(name);
    return !trimmed.empty() && !std::isdigit(static_cast<unsigned char>(trimmed.front())) &&
        std::all_of(trimmed.begin(), trimmed.end(), IsIdentifierChar);
}

std::optional<size_t> FindMatchingSourceParen(const std::string& text, size_t open)
{
    if (open >= text.size() || text[open] != '(') {
        return std::nullopt;
    }
    int depth = 0;
    for (size_t pos = open; pos < text.size(); ++pos) {
        if (text[pos] == '(') {
            ++depth;
        } else if (text[pos] == ')') {
            --depth;
            if (depth == 0) {
                return pos;
            }
        }
    }
    return std::nullopt;
}

RelationalOperation ReverseSourceRelation(RelationalOperation relation)
{
    switch (relation) {
        case RelationalOperation::GE:
            return RelationalOperation::LE;
        case RelationalOperation::LE:
            return RelationalOperation::GE;
        case RelationalOperation::GT:
            return RelationalOperation::LT;
        case RelationalOperation::LT:
            return RelationalOperation::GT;
        case RelationalOperation::EQ:
            return RelationalOperation::EQ;
        case RelationalOperation::NE:
            return RelationalOperation::NE;
    }
    return relation;
}

bool SourceRelationHolds(int64_t lhs, RelationalOperation relation, int64_t rhs)
{
    switch (relation) {
        case RelationalOperation::GE:
            return lhs >= rhs;
        case RelationalOperation::LE:
            return lhs <= rhs;
        case RelationalOperation::EQ:
            return lhs == rhs;
        case RelationalOperation::NE:
            return lhs != rhs;
        case RelationalOperation::GT:
            return lhs > rhs;
        case RelationalOperation::LT:
            return lhs < rhs;
    }
    return false;
}

std::optional<SourceVariableComparison> ParseSourceVariableComparison(const std::string& condition)
{
    auto trimmed = StripSourceEnclosingParens(condition);
    const std::vector<std::pair<std::string, RelationalOperation>> relations{
        {">=", RelationalOperation::GE}, {"<=", RelationalOperation::LE}, {"==", RelationalOperation::EQ},
        {"!=", RelationalOperation::NE}, {">", RelationalOperation::GT}, {"<", RelationalOperation::LT}};
    for (const auto& relation : relations) {
        auto pos = FindSourceBinaryToken(trimmed, relation.first);
        if (!pos.has_value()) {
            continue;
        }
        auto lhsName = StripSourceEnclosingParens(trimmed.substr(0, pos.value()));
        auto rhsName = StripSourceEnclosingParens(trimmed.substr(pos.value() + relation.first.size()));
        if (!IsSourceIdentifierName(lhsName) || !IsSourceIdentifierName(rhsName)) {
            return std::nullopt;
        }
        return SourceVariableComparison{lhsName, rhsName, relation.second};
    }
    return std::nullopt;
}

std::optional<std::string> ParseSourceWhileCondition(const std::string& line)
{
    auto header = Trim(StripLineComment(line));
    if (!SourceLineStartsWithKeyword(header, "while")) {
        return std::nullopt;
    }
    auto open = header.find('(');
    if (open == std::string::npos) {
        return std::nullopt;
    }
    auto close = FindMatchingSourceParen(header, open);
    if (!close.has_value() || close.value() <= open) {
        return std::nullopt;
    }
    return header.substr(open + 1, close.value() - open - 1);
}

std::optional<SourceVariableComparison> ParseSourcePairWhileHeader(const std::string& line)
{
    auto condition = ParseSourceWhileCondition(line);
    if (!condition.has_value()) {
        return std::nullopt;
    }
    auto comparison = ParseSourceVariableComparison(condition.value());
    if (!comparison.has_value() || comparison->lhs == comparison->rhs) {
        return std::nullopt;
    }
    if (comparison->relation != RelationalOperation::LT && comparison->relation != RelationalOperation::LE &&
        comparison->relation != RelationalOperation::GT && comparison->relation != RelationalOperation::GE) {
        return std::nullopt;
    }
    return comparison;
}

std::optional<std::vector<int64_t>> NormalizeSmallSourceValues(std::vector<int64_t> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty() || values.size() > MAX_CONTEST_EXACT_VALUES) {
        return std::nullopt;
    }
    return values;
}

std::optional<std::vector<int64_t>> InferSourceNameValuesAtLine(
    const std::vector<std::string>& lines, unsigned line, const std::string& name, unsigned depth);

std::optional<std::vector<int64_t>> EvalSourceIntExprValuesAtLine(
    const std::vector<std::string>& lines, unsigned line, const std::string& expr, unsigned depth)
{
    constexpr unsigned MAX_SOURCE_VALUE_RECURSION_DEPTH = 8;
    if (depth > MAX_SOURCE_VALUE_RECURSION_DEPTH) {
        return std::nullopt;
    }
    auto trimmed = StripSourceEnclosingParens(expr);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed = Trim(trimmed.substr(0, trimmed.size() - 1));
    }
    std::vector<SourceScope> emptyScopes(1);
    if (auto literal = EvalSourceIntExpr(trimmed, emptyScopes); literal.has_value()) {
        return std::vector<int64_t>{literal.value()};
    }
    if (IsSourceIdentifierName(trimmed)) {
        return InferSourceNameValuesAtLine(lines, line, trimmed, depth + 1);
    }
    for (const auto& token : {std::string("+"), std::string("-")}) {
        auto pos = FindSourceBinaryToken(trimmed, token);
        if (!pos.has_value()) {
            continue;
        }
        auto lhsValues = EvalSourceIntExprValuesAtLine(lines, line, trimmed.substr(0, pos.value()), depth + 1);
        auto rhsValues = EvalSourceIntExprValuesAtLine(
            lines, line, trimmed.substr(pos.value() + token.size()), depth + 1);
        if (!lhsValues.has_value() || !rhsValues.has_value()) {
            return std::nullopt;
        }
        std::vector<int64_t> values;
        for (auto lhsValue : lhsValues.value()) {
            for (auto rhsValue : rhsValues.value()) {
                auto result = EvalSourceBinaryIntOp(lhsValue, rhsValue, token);
                if (!result.has_value()) {
                    return std::nullopt;
                }
                values.emplace_back(result.value());
            }
        }
        return NormalizeSmallSourceValues(std::move(values));
    }
    return std::nullopt;
}

std::vector<SourceLoopExtent> CollectSourceLoopExtents(const std::vector<std::string>& lines)
{
    std::vector<SourceLoopExtent> loops;
    for (unsigned lineNo = 1; lineNo <= lines.size(); ++lineNo) {
        if (!SourceLineStartsLoop(lines[lineNo - 1])) {
            continue;
        }
        auto end = FindSourceBraceBlockEnd(lines, lineNo);
        if (end.has_value() && end.value() >= lineNo) {
            loops.emplace_back(SourceLoopExtent{lineNo, end.value()});
        }
    }
    return loops;
}

bool SourceLineIsInsideLoopExtent(const SourceLoopExtent& loop, unsigned line)
{
    return loop.start < line && line <= loop.end;
}

std::optional<int64_t> ParseSourceSelfStepExpr(const std::string& variableName, const std::string& expr)
{
    auto trimmed = StripSourceEnclosingParens(expr);
    if (trimmed == variableName) {
        return 0;
    }
    std::vector<SourceScope> emptyScopes(1);
    if (auto pos = FindSourceBinaryToken(trimmed, "+"); pos.has_value()) {
        auto lhs = StripSourceEnclosingParens(trimmed.substr(0, pos.value()));
        auto rhs = StripSourceEnclosingParens(trimmed.substr(pos.value() + 1));
        if (lhs == variableName) {
            return EvalSourceIntExpr(rhs, emptyScopes);
        }
        if (rhs == variableName) {
            return EvalSourceIntExpr(lhs, emptyScopes);
        }
        return std::nullopt;
    }
    if (auto pos = FindSourceBinaryToken(trimmed, "-"); pos.has_value()) {
        auto lhs = StripSourceEnclosingParens(trimmed.substr(0, pos.value()));
        auto rhs = StripSourceEnclosingParens(trimmed.substr(pos.value() + 1));
        if (lhs != variableName) {
            return std::nullopt;
        }
        auto delta = EvalSourceIntExpr(rhs, emptyScopes);
        if (!delta.has_value()) {
            return std::nullopt;
        }
        if (delta.value() == std::numeric_limits<int64_t>::min()) {
            return std::nullopt;
        }
        return -delta.value();
    }
    return std::nullopt;
}

std::optional<int64_t> ParseSourceIncDecStep(const std::string& line, const std::string& variableName)
{
    auto trimmed = Trim(StripLineComment(line));
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed = Trim(trimmed.substr(0, trimmed.size() - 1));
    }
    if (trimmed == variableName + "++" || trimmed == "++" + variableName) {
        return 1;
    }
    if (trimmed == variableName + "--" || trimmed == "--" + variableName) {
        return -1;
    }
    return std::nullopt;
}

std::optional<int64_t> FindSourceLoopVariableStep(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    std::optional<int64_t> step;
    for (unsigned lineNo = loop.start + 1; lineNo < loop.end && lineNo <= lines.size(); ++lineNo) {
        std::string declared;
        ContestQueryTypeHint declaredType{ContestQueryTypeHint::UNKNOWN};
        std::string declaredExpr;
        if (ParseSourceDeclaration(lines[lineNo - 1], declared, declaredType, declaredExpr) &&
            declared == variableName) {
            return std::nullopt;
        }
        std::optional<int64_t> current;
        std::string assigned;
        std::string assignedExpr;
        if (ParseSourceAssignment(lines[lineNo - 1], assigned, assignedExpr) && assigned == variableName) {
            current = ParseSourceSelfStepExpr(variableName, assignedExpr);
            if (!current.has_value()) {
                return std::nullopt;
            }
        } else {
            std::string op;
            if (ParseSourceCompoundAssignment(lines[lineNo - 1], assigned, op, assignedExpr) &&
                assigned == variableName) {
                std::vector<SourceScope> emptyScopes(1);
                auto rhsValue = EvalSourceIntExpr(assignedExpr, emptyScopes);
                if (!rhsValue.has_value()) {
                    return std::nullopt;
                }
                if (op == "+") {
                    current = rhsValue.value();
                } else if (op == "-") {
                    if (rhsValue.value() == std::numeric_limits<int64_t>::min()) {
                        return std::nullopt;
                    }
                    current = -rhsValue.value();
                } else {
                    return std::nullopt;
                }
            } else {
                current = ParseSourceIncDecStep(lines[lineNo - 1], variableName);
            }
        }
        if (!current.has_value()) {
            continue;
        }
        if (step.has_value()) {
            return std::nullopt;
        }
        step = current;
    }
    return step.value_or(0);
}

std::optional<unsigned> FindSourceLoopVariableUpdateLine(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    std::optional<unsigned> updateLine;
    for (unsigned lineNo = loop.start + 1; lineNo < loop.end && lineNo <= lines.size(); ++lineNo) {
        std::string declared;
        ContestQueryTypeHint declaredType{ContestQueryTypeHint::UNKNOWN};
        std::string declaredExpr;
        if (ParseSourceDeclaration(lines[lineNo - 1], declared, declaredType, declaredExpr) &&
            declared == variableName) {
            return std::nullopt;
        }

        bool updatesVariable = false;
        std::string assigned;
        std::string assignedExpr;
        if (ParseSourceAssignment(lines[lineNo - 1], assigned, assignedExpr) && assigned == variableName) {
            updatesVariable = true;
        } else {
            std::string op;
            updatesVariable =
                (ParseSourceCompoundAssignment(lines[lineNo - 1], assigned, op, assignedExpr) &&
                    assigned == variableName) ||
                ParseSourceIncDecStep(lines[lineNo - 1], variableName).has_value();
        }
        if (!updatesVariable) {
            continue;
        }
        if (updateLine.has_value()) {
            return std::nullopt;
        }
        updateLine = lineNo;
    }
    return updateLine;
}

std::optional<std::vector<int64_t>> GetSourceForRangeValues(const SourceForRangeInfo& range)
{
    auto bounds = GetSourceForRangeBounds(range);
    if (!bounds.has_value()) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    for (int64_t value = bounds->first;; ++value) {
        values.emplace_back(value);
        if (values.size() > MAX_CONTEST_EXACT_VALUES) {
            return std::nullopt;
        }
        if (value == bounds->second) {
            break;
        }
    }
    return values;
}

std::optional<std::vector<int64_t>> InferSourceWhileInductionValues(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& name, unsigned depth)
{
    auto condition = ParseSourceWhileCondition(lines[loop.start - 1]);
    if (!condition.has_value()) {
        return std::nullopt;
    }
    auto trimmed = StripSourceEnclosingParens(condition.value());
    const std::vector<std::pair<std::string, RelationalOperation>> relations{
        {">=", RelationalOperation::GE}, {"<=", RelationalOperation::LE},
        {">", RelationalOperation::GT}, {"<", RelationalOperation::LT}};
    std::optional<RelationalOperation> relation;
    std::optional<int64_t> bound;
    for (const auto& relationInfo : relations) {
        auto pos = FindSourceBinaryToken(trimmed, relationInfo.first);
        if (!pos.has_value()) {
            continue;
        }
        auto lhs = StripSourceEnclosingParens(trimmed.substr(0, pos.value()));
        auto rhs = StripSourceEnclosingParens(trimmed.substr(pos.value() + relationInfo.first.size()));
        if (lhs == name) {
            auto values = EvalSourceIntExprValuesAtLine(lines, loop.start, rhs, depth + 1);
            if (!values.has_value() || values->size() != 1) {
                return std::nullopt;
            }
            relation = relationInfo.second;
            bound = values->front();
            break;
        }
        if (rhs == name) {
            auto values = EvalSourceIntExprValuesAtLine(lines, loop.start, lhs, depth + 1);
            if (!values.has_value() || values->size() != 1) {
                return std::nullopt;
            }
            relation = ReverseSourceRelation(relationInfo.second);
            bound = values->front();
            break;
        }
    }
    if (!relation.has_value() || !bound.has_value()) {
        return std::nullopt;
    }
    auto initValues = InferSourceNameValuesAtLine(lines, loop.start, name, depth + 1);
    auto step = FindSourceLoopVariableStep(lines, loop, name);
    if (!initValues.has_value() || !step.has_value() || step.value() == 0) {
        return std::nullopt;
    }
    std::vector<int64_t> values;
    for (auto init : initValues.value()) {
        int64_t current = init;
        size_t guard = 0;
        while (SourceRelationHolds(current, relation.value(), bound.value())) {
            values.emplace_back(current);
            if (values.size() > MAX_CONTEST_EXACT_VALUES || ++guard > MAX_CONTEST_EXACT_VALUES) {
                return std::nullopt;
            }
            int64_t next = 0;
            if (__builtin_add_overflow(current, step.value(), &next)) {
                return std::nullopt;
            }
            current = next;
        }
        values.emplace_back(current);
        if (values.size() > MAX_CONTEST_EXACT_VALUES) {
            return std::nullopt;
        }
    }
    return NormalizeSmallSourceValues(std::move(values));
}

std::optional<std::vector<int64_t>> InferSourceNameValuesAtLine(
    const std::vector<std::string>& lines, unsigned line, const std::string& name, unsigned depth)
{
    constexpr unsigned MAX_SOURCE_VALUE_RECURSION_DEPTH = 8;
    if (depth > MAX_SOURCE_VALUE_RECURSION_DEPTH || line == 0 || !IsSourceIdentifierName(name)) {
        return std::nullopt;
    }
    auto loops = CollectSourceLoopExtents(lines);
    std::sort(loops.begin(), loops.end(), [](const SourceLoopExtent& lhs, const SourceLoopExtent& rhs) {
        if (lhs.start != rhs.start) {
            return lhs.start > rhs.start;
        }
        return lhs.end < rhs.end;
    });
    for (const auto& loop : loops) {
        if (!SourceLineIsInsideLoopExtent(loop, line)) {
            continue;
        }
        if (auto range = ParseSourceForInRangeHeader(lines[loop.start - 1]);
            range.has_value() && range->variable == name) {
            return GetSourceForRangeValues(range.value());
        }
        if (auto values = InferSourceWhileInductionValues(lines, loop, name, depth + 1);
            values.has_value()) {
            return values;
        }
    }

    unsigned lineNo = std::min<unsigned>(line > 0 ? line - 1 : 0, static_cast<unsigned>(lines.size()));
    while (lineNo >= 1) {
        std::string declared;
        ContestQueryTypeHint declaredType{ContestQueryTypeHint::UNKNOWN};
        std::string declaredExpr;
        if (ParseSourceDeclaration(lines[lineNo - 1], declared, declaredType, declaredExpr) && declared == name) {
            return EvalSourceIntExprValuesAtLine(lines, lineNo, declaredExpr, depth + 1);
        }
        std::string assigned;
        std::string assignedExpr;
        if (ParseSourceAssignment(lines[lineNo - 1], assigned, assignedExpr) && assigned == name) {
            return EvalSourceIntExprValuesAtLine(lines, lineNo, assignedExpr, depth + 1);
        }
        if (lineNo == 1) {
            break;
        }
        --lineNo;
    }
    return std::nullopt;
}

std::optional<SourcePairLoopInfo> BuildSourcePairLoopInfo(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop)
{
    if (loop.start == 0 || loop.start > lines.size()) {
        return std::nullopt;
    }
    auto comparison = ParseSourcePairWhileHeader(lines[loop.start - 1]);
    if (!comparison.has_value()) {
        return std::nullopt;
    }
    auto lhsStep = FindSourceLoopVariableStep(lines, loop, comparison->lhs);
    auto rhsStep = FindSourceLoopVariableStep(lines, loop, comparison->rhs);
    if (!lhsStep.has_value() || !rhsStep.has_value()) {
        return std::nullopt;
    }
    auto relativeStep = static_cast<__int128>(lhsStep.value()) - static_cast<__int128>(rhsStep.value());
    if (((comparison->relation == RelationalOperation::LT || comparison->relation == RelationalOperation::LE) &&
            relativeStep <= 0) ||
        ((comparison->relation == RelationalOperation::GT || comparison->relation == RelationalOperation::GE) &&
            relativeStep >= 0)) {
        return std::nullopt;
    }
    auto lhsInits = InferSourceNameValuesAtLine(lines, loop.start, comparison->lhs, 0);
    auto rhsInits = InferSourceNameValuesAtLine(lines, loop.start, comparison->rhs, 0);
    if (!lhsInits.has_value() || !rhsInits.has_value()) {
        return std::nullopt;
    }
    return SourcePairLoopInfo{
        loop, comparison.value(), lhsStep.value(), rhsStep.value(), lhsInits.value(), rhsInits.value()};
}

std::optional<SourcePairLoopValues> EnumerateSourcePairLoopValues(const SourcePairLoopInfo& info)
{
    SourcePairLoopValues values;
    for (auto lhsInit : info.lhsInits) {
        for (auto rhsInit : info.rhsInits) {
            int64_t lhsValue = lhsInit;
            int64_t rhsValue = rhsInit;
            size_t guard = 0;
            while (SourceRelationHolds(lhsValue, info.comparison.relation, rhsValue)) {
                values.lhsHeaderValues.emplace_back(lhsValue);
                values.rhsHeaderValues.emplace_back(rhsValue);
                values.lhsBodyValues.emplace_back(lhsValue);
                values.rhsBodyValues.emplace_back(rhsValue);
                int64_t nextLhs = 0;
                int64_t nextRhs = 0;
                if (__builtin_add_overflow(lhsValue, info.lhsStep, &nextLhs) ||
                    __builtin_add_overflow(rhsValue, info.rhsStep, &nextRhs)) {
                    return std::nullopt;
                }
                values.lhsPostBodyValues.emplace_back(nextLhs);
                values.rhsPostBodyValues.emplace_back(nextRhs);
                lhsValue = nextLhs;
                rhsValue = nextRhs;
                if (++guard > MAX_CONTEST_EXACT_VALUES) {
                    return std::nullopt;
                }
            }
            values.lhsHeaderValues.emplace_back(lhsValue);
            values.rhsHeaderValues.emplace_back(rhsValue);
            values.lhsExitValues.emplace_back(lhsValue);
            values.rhsExitValues.emplace_back(rhsValue);
        }
    }
    auto normalize = [](std::vector<int64_t>& target) -> bool {
        auto normalized = NormalizeSmallSourceValues(std::move(target));
        if (!normalized.has_value()) {
            return false;
        }
        target = std::move(normalized.value());
        return true;
    };
    if (!normalize(values.lhsHeaderValues) || !normalize(values.rhsHeaderValues) ||
        !normalize(values.lhsExitValues) || !normalize(values.rhsExitValues)) {
        return std::nullopt;
    }
    if (!values.lhsBodyValues.empty() && !normalize(values.lhsBodyValues)) {
        return std::nullopt;
    }
    if (!values.rhsBodyValues.empty() && !normalize(values.rhsBodyValues)) {
        return std::nullopt;
    }
    if (!values.lhsPostBodyValues.empty() && !normalize(values.lhsPostBodyValues)) {
        return std::nullopt;
    }
    if (!values.rhsPostBodyValues.empty() && !normalize(values.rhsPostBodyValues)) {
        return std::nullopt;
    }
    return values;
}

std::optional<bool> GetSourcePairLoopTargetSide(
    const std::vector<std::string>& lines, const ContestQuery& query, const SourcePairLoopInfo& info)
{
    if (query.variableName == info.comparison.lhs) {
        return true;
    }
    if (query.variableName == info.comparison.rhs) {
        return false;
    }
    if (query.line == 0 || query.line > lines.size()) {
        return std::nullopt;
    }
    std::string assigned;
    std::string assignedExpr;
    ContestQueryTypeHint assignedType{ContestQueryTypeHint::UNKNOWN};
    bool matched = false;
    if (ParseSourceDeclaration(lines[query.line - 1], assigned, assignedType, assignedExpr) &&
        assigned == query.variableName) {
        matched = true;
    } else if (ParseSourceAssignment(lines[query.line - 1], assigned, assignedExpr) &&
        assigned == query.variableName) {
        matched = true;
    }
    if (!matched) {
        return std::nullopt;
    }
    auto expr = StripSourceEnclosingParens(assignedExpr);
    if (expr == info.comparison.lhs) {
        return true;
    }
    if (expr == info.comparison.rhs) {
        return false;
    }
    return std::nullopt;
}

bool SourceHasAssignmentInRange(
    const std::vector<std::string>& lines, unsigned begin, unsigned end, const std::string& variableName)
{
    if (begin > end) {
        return false;
    }
    auto limit = std::min<unsigned>(end, static_cast<unsigned>(lines.size()));
    for (unsigned lineNo = begin; lineNo <= limit; ++lineNo) {
        if (SourceLineAssignsVariableName(lines[lineNo - 1], variableName)) {
            return true;
        }
    }
    return false;
}

std::optional<int64_t> ComputeSourceLinearLoopExitValue(
    int64_t init, RelationalOperation relation, int64_t bound, int64_t step)
{
    if (!SourceRelationHolds(init, relation, bound)) {
        return init;
    }

    auto ceilDivPositive = [](const __int128 numerator, const __int128 denominator) -> std::optional<__int128> {
        if (numerator <= 0 || denominator <= 0) {
            return std::nullopt;
        }
        return (numerator + denominator - 1) / denominator;
    };

    __int128 current = static_cast<__int128>(init);
    __int128 limit = static_cast<__int128>(bound);
    __int128 delta = static_cast<__int128>(step);
    std::optional<__int128> iterations;
    switch (relation) {
        case RelationalOperation::LT:
            if (delta <= 0) {
                return std::nullopt;
            }
            iterations = ceilDivPositive(limit - current, delta);
            break;
        case RelationalOperation::LE:
            if (delta <= 0 || bound == std::numeric_limits<int64_t>::max()) {
                return std::nullopt;
            }
            iterations = ceilDivPositive(limit + 1 - current, delta);
            break;
        case RelationalOperation::GT:
            if (delta >= 0) {
                return std::nullopt;
            }
            iterations = ceilDivPositive(current - limit, -delta);
            break;
        case RelationalOperation::GE:
            if (delta >= 0 || bound == std::numeric_limits<int64_t>::min()) {
                return std::nullopt;
            }
            iterations = ceilDivPositive(current - (limit - 1), -delta);
            break;
        case RelationalOperation::EQ:
        case RelationalOperation::NE:
            return std::nullopt;
    }
    if (!iterations.has_value()) {
        return std::nullopt;
    }

    __int128 exitValue = current + iterations.value() * delta;
    if (exitValue < static_cast<__int128>(std::numeric_limits<int64_t>::min()) ||
        exitValue > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    auto result = static_cast<int64_t>(exitValue);
    if (SourceRelationHolds(result, relation, bound)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<int64_t>> InferSourceLinearLoopExitValues(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    if (loop.start == 0 || loop.start > lines.size()) {
        return std::nullopt;
    }
    auto condition = ParseSourceWhileCondition(lines[loop.start - 1]);
    if (!condition.has_value()) {
        return std::nullopt;
    }
    auto trimmed = StripSourceEnclosingParens(condition.value());
    const std::vector<std::pair<std::string, RelationalOperation>> relations{
        {">=", RelationalOperation::GE}, {"<=", RelationalOperation::LE},
        {">", RelationalOperation::GT}, {"<", RelationalOperation::LT}};

    std::optional<RelationalOperation> relation;
    std::optional<int64_t> bound;
    for (const auto& relationInfo : relations) {
        auto pos = FindSourceBinaryToken(trimmed, relationInfo.first);
        if (!pos.has_value()) {
            continue;
        }
        auto lhs = StripSourceEnclosingParens(trimmed.substr(0, pos.value()));
        auto rhs = StripSourceEnclosingParens(trimmed.substr(pos.value() + relationInfo.first.size()));
        if (lhs == variableName) {
            auto values = EvalSourceIntExprValuesAtLine(lines, loop.start, rhs, 0);
            if (!values.has_value() || values->size() != 1) {
                return std::nullopt;
            }
            relation = relationInfo.second;
            bound = values->front();
            break;
        }
        if (rhs == variableName) {
            auto values = EvalSourceIntExprValuesAtLine(lines, loop.start, lhs, 0);
            if (!values.has_value() || values->size() != 1) {
                return std::nullopt;
            }
            relation = ReverseSourceRelation(relationInfo.second);
            bound = values->front();
            break;
        }
    }
    if (!relation.has_value() || !bound.has_value()) {
        return std::nullopt;
    }

    auto initValues = InferSourceNameValuesAtLine(lines, loop.start, variableName, 0);
    auto step = FindSourceLoopVariableStep(lines, loop, variableName);
    if (!initValues.has_value() || !step.has_value() || step.value() == 0) {
        return std::nullopt;
    }

    std::vector<int64_t> exits;
    exits.reserve(initValues->size());
    for (auto init : initValues.value()) {
        auto exitValue = ComputeSourceLinearLoopExitValue(init, relation.value(), bound.value(), step.value());
        if (!exitValue.has_value()) {
            return std::nullopt;
        }
        exits.emplace_back(exitValue.value());
    }
    return NormalizeSmallSourceValues(std::move(exits));
}

std::optional<std::vector<int64_t>> InferSourceLoopExitValuesForVariable(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    if (auto info = BuildSourcePairLoopInfo(lines, loop); info.has_value()) {
        if (auto values = EnumerateSourcePairLoopValues(info.value()); values.has_value()) {
            if (variableName == info->comparison.lhs) {
                return values->lhsExitValues;
            }
            if (variableName == info->comparison.rhs) {
                return values->rhsExitValues;
            }
        }
    }
    return InferSourceLinearLoopExitValues(lines, loop, variableName);
}

std::optional<std::vector<int64_t>> InferSourceLoopAllValuesForVariable(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    if (auto info = BuildSourcePairLoopInfo(lines, loop); info.has_value()) {
        if (auto values = EnumerateSourcePairLoopValues(info.value()); values.has_value()) {
            std::vector<int64_t> selected;
            if (variableName == info->comparison.lhs) {
                selected = values->lhsBodyValues;
                selected.insert(selected.end(), values->lhsPostBodyValues.begin(), values->lhsPostBodyValues.end());
                selected.insert(selected.end(), values->lhsExitValues.begin(), values->lhsExitValues.end());
                return NormalizeSmallSourceValues(std::move(selected));
            }
            if (variableName == info->comparison.rhs) {
                selected = values->rhsBodyValues;
                selected.insert(selected.end(), values->rhsPostBodyValues.begin(), values->rhsPostBodyValues.end());
                selected.insert(selected.end(), values->rhsExitValues.begin(), values->rhsExitValues.end());
                return NormalizeSmallSourceValues(std::move(selected));
            }
        }
    }
    return InferSourceWhileInductionValues(lines, loop, variableName, 0);
}

std::optional<std::string> InferSourcePriorLoopAccumulatorOutput(
    const std::vector<std::string>& lines, const SourceLoopExtent& loop, const std::string& variableName)
{
    auto init = InferSourceAccumulatorInit(lines, loop, variableName);
    auto tripCount = ParseSourceTripCount(lines, loop);
    if (!init.has_value() || !tripCount.has_value() || tripCount.value() <= 0) {
        return std::nullopt;
    }

    std::optional<std::pair<int64_t, int64_t>> delta;
    for (unsigned lineNo = loop.start + 1; lineNo < loop.end && lineNo <= lines.size(); ++lineNo) {
        std::string assigned;
        std::string expr;
        if (!ParseSourceAssignment(lines[lineNo - 1], assigned, expr) || assigned != variableName) {
            continue;
        }
        if (auto exactValues = InferSourceAccumulatorExactValues(lines, loop, variableName, init.value(), lineNo);
            exactValues.has_value() && !exactValues->empty() && exactValues->size() <= MAX_CONTEST_EXACT_VALUES) {
            auto normalized = NormalizeSourceIntSet(std::move(exactValues.value()));
            if (normalized.has_value()) {
                return FormatSourceIntValues(std::move(normalized.value()));
            }
        }
        auto current = InferSourceDeltaInterval(lines, loop, variableName, expr, lineNo);
        if (!current.has_value()) {
            continue;
        }
        if (!delta.has_value()) {
            delta = current;
        } else {
            delta->first = std::min(delta->first, current->first);
            delta->second = std::max(delta->second, current->second);
        }
    }
    if (!delta.has_value()) {
        return std::nullopt;
    }

    std::vector<int64_t> candidates{init.value()};
    for (auto step : {delta->first, delta->second}) {
        for (auto count : {tripCount.value() - 1, tripCount.value()}) {
            int64_t value = 0;
            if (count >= 0 && !__builtin_mul_overflow(count, step, &value) &&
                !__builtin_add_overflow(init.value(), value, &value)) {
                candidates.emplace_back(value);
            }
        }
    }
    auto [lowerIt, upperIt] = std::minmax_element(candidates.begin(), candidates.end());
    return "[" + std::to_string(*lowerIt) + ", " + std::to_string(*upperIt) + ":1]";
}

bool SourceHasUnsafePriorLoopAssignment(
    const std::vector<std::string>& lines, unsigned line, const std::string& variableName)
{
    if (line == 0 || line > lines.size() || IsSourceLineInsideLoop(lines, line)) {
        return false;
    }
    auto loops = CollectSourceLoopExtents(lines);
    for (const auto& loop : loops) {
        if (loop.end >= line || !SourceHasAssignmentInRange(lines, loop.start + 1, loop.end - 1, variableName)) {
            continue;
        }
        if (SourceHasAssignmentInRange(lines, loop.end + 1, line - 1, variableName)) {
            continue;
        }
        const bool nestedInPriorLoop = std::any_of(loops.begin(), loops.end(), [&loop, line](const SourceLoopExtent& outer) {
            return outer.start < loop.start && loop.end < outer.end && outer.end < line;
        });
        if (nestedInPriorLoop) {
            return true;
        }
        auto exitValues = InferSourceLoopExitValuesForVariable(lines, loop, variableName);
        if (!exitValues.has_value() || exitValues->empty()) {
            return true;
        }
    }
    return false;
}

void InferSourcePriorLoopValueFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL ||
        IsSourceLineInsideLoop(lines, query.line)) {
        return;
    }
    auto loops = CollectSourceLoopExtents(lines);
    std::sort(loops.begin(), loops.end(), [](const SourceLoopExtent& lhs, const SourceLoopExtent& rhs) {
        if (lhs.end != rhs.end) {
            return lhs.end > rhs.end;
        }
        return lhs.start > rhs.start;
    });
    for (const auto& loop : loops) {
        if (loop.end >= query.line ||
            !SourceHasAssignmentInRange(lines, loop.start + 1, loop.end - 1, query.variableName) ||
            SourceHasAssignmentInRange(lines, loop.end + 1, query.line - 1, query.variableName)) {
            continue;
        }
        auto values = InferSourceLoopAllValuesForVariable(lines, loop, query.variableName);
        if (!values.has_value() || values->empty()) {
            if (auto accumulatorOutput = InferSourcePriorLoopAccumulatorOutput(lines, loop, query.variableName);
                accumulatorOutput.has_value()) {
                query.sourceFallback = std::move(accumulatorOutput.value());
                query.hasSourceFallback = true;
                query.preferSourceFallback = true;
                query.sourceFallbackMayBeLoopNarrow = true;
                return;
            }
            continue;
        }
        SetSourceIntSetFallback(query, std::move(values.value()));
        query.preferSourceFallback = query.hasSourceFallback;
        query.sourceFallbackMayBeLoopNarrow = query.hasSourceFallback;
        return;
    }
}

void InferSourcePairLoopFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    auto loops = CollectSourceLoopExtents(lines);
    std::sort(loops.begin(), loops.end(), [](const SourceLoopExtent& lhs, const SourceLoopExtent& rhs) {
        if (lhs.start != rhs.start) {
            return lhs.start > rhs.start;
        }
        return lhs.end < rhs.end;
    });
    for (const auto& loop : loops) {
        if (loop.start > query.line) {
            continue;
        }
        auto info = BuildSourcePairLoopInfo(lines, loop);
        if (!info.has_value()) {
            continue;
        }
        auto targetSide = GetSourcePairLoopTargetSide(lines, query, info.value());
        if (!targetSide.has_value()) {
            continue;
        }
        const bool queryOnHeader = query.line == loop.start;
        const bool queryInsideBody = loop.start < query.line && query.line < loop.end;
        const bool queryAfterLoop = query.line > loop.end;
        if (!queryOnHeader && !queryInsideBody && !queryAfterLoop) {
            continue;
        }
        if (queryAfterLoop && (query.variableName == info->comparison.lhs || query.variableName == info->comparison.rhs) &&
            SourceHasAssignmentInRange(lines, loop.end + 1, query.line - 1, query.variableName)) {
            continue;
        }
        auto values = EnumerateSourcePairLoopValues(info.value());
        if (!values.has_value()) {
            continue;
        }
        std::vector<int64_t> selected;
        if (queryOnHeader) {
            selected = targetSide.value() ? values->lhsHeaderValues : values->rhsHeaderValues;
        } else if (queryInsideBody) {
            auto updateLine = FindSourceLoopVariableUpdateLine(
                lines, loop, targetSide.value() ? info->comparison.lhs : info->comparison.rhs);
            if (updateLine.has_value() && updateLine.value() <= query.line) {
                selected = targetSide.value() ? values->lhsPostBodyValues : values->rhsPostBodyValues;
            } else {
                selected = targetSide.value() ? values->lhsBodyValues : values->rhsBodyValues;
            }
        } else {
            selected = targetSide.value() ? values->lhsExitValues : values->rhsExitValues;
        }
        if (!selected.empty()) {
            SetSourceIntSetFallback(query, std::move(selected));
            query.preferSourceFallback = query.hasSourceFallback;
            query.sourceFallbackMayBeLoopNarrow = query.hasSourceFallback;
            query.hasLineSensitiveLoopFallback = query.hasSourceFallback && queryInsideBody;
            return;
        }
    }
}

struct SourceFunctionSummary {
    std::vector<std::string> params;
    std::vector<std::string> returnExprs;
    std::vector<std::string> bodyLines;
    unsigned startLine{0};
    unsigned endLine{0};
};

std::vector<std::string> SplitSourceTopLevelCommaList(const std::string& text)
{
    std::vector<std::string> parts;
    int depth = 0;
    size_t begin = 0;
    for (size_t pos = 0; pos <= text.size(); ++pos) {
        if (pos == text.size() || (text[pos] == ',' && depth == 0)) {
            auto part = Trim(text.substr(begin, pos - begin));
            if (!part.empty()) {
                parts.emplace_back(part);
            }
            begin = pos + 1;
            continue;
        }
        if (text[pos] == '(' || text[pos] == '<' || text[pos] == '{' || text[pos] == '[') {
            ++depth;
        } else if ((text[pos] == ')' || text[pos] == '>' || text[pos] == '}' || text[pos] == ']') && depth > 0) {
            --depth;
        }
    }
    return parts;
}

std::vector<std::string> ParseSourceParamNames(const std::string& text)
{
    std::vector<std::string> params;
    for (auto param : SplitSourceTopLevelCommaList(text)) {
        auto colon = param.find(':');
        auto name = Trim(colon == std::string::npos ? param : param.substr(0, colon));
        if (!name.empty() && std::all_of(name.begin(), name.end(), IsIdentifierChar)) {
            params.emplace_back(name);
        }
    }
    return params;
}

std::optional<SourceFunctionSummary> ParseSourceLambdaSummary(const std::string& expr)
{
    auto trimmed = Trim(StripLineComment(expr));
    if (trimmed.size() < 4 || trimmed.front() != '{') {
        return std::nullopt;
    }
    auto close = trimmed.rfind('}');
    if (close == std::string::npos) {
        return std::nullopt;
    }
    auto inner = Trim(trimmed.substr(1, close - 1));
    auto arrow = inner.find("=>");
    if (arrow == std::string::npos) {
        return std::nullopt;
    }
    SourceFunctionSummary summary;
    auto params = Trim(inner.substr(0, arrow));
    auto afterGeneric = SkipSourceGenericParameterList(params, 0);
    if (afterGeneric > 0 && afterGeneric <= params.size()) {
        params = Trim(params.substr(afterGeneric));
    }
    if (HasSourceEnclosingParens(params)) {
        params = StripSourceEnclosingParens(params);
    }
    summary.params = ParseSourceParamNames(params);
    auto ret = TrimSourceMatchArmExpr(inner.substr(arrow + 2));
    if (!ret.empty()) {
        summary.returnExprs.emplace_back(std::move(ret));
    }
    return summary.returnExprs.empty() ? std::nullopt : std::optional<SourceFunctionSummary>(std::move(summary));
}

std::string StripSourceGenericSuffix(std::string callee)
{
    callee = Trim(callee);
    auto generic = callee.find('<');
    if (generic != std::string::npos) {
        callee = callee.substr(0, generic);
    }
    auto dot = callee.find_last_of('.');
    if (dot != std::string::npos) {
        callee = callee.substr(dot + 1);
    }
    return Trim(callee);
}

std::optional<std::string> ExtractSourceReturnExpr(const std::string& line)
{
    auto trimmed = Trim(StripLineComment(line));
    auto pos = trimmed.find("return");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto before = pos == 0 ? '\0' : trimmed[pos - 1];
    auto afterPos = pos + 6;
    auto after = afterPos >= trimmed.size() ? '\0' : trimmed[afterPos];
    if (IsIdentifierChar(before) || IsIdentifierChar(after)) {
        return std::nullopt;
    }
    return TrimSourceMatchArmExpr(trimmed.substr(afterPos));
}

std::optional<std::string> ExtractSourceExpressionResultExpr(const std::string& line)
{
    auto trimmed = TrimSourceMatchArmExpr(line);
    if (trimmed.empty() || trimmed == "{" || trimmed == "}" || trimmed.back() == ':') {
        return std::nullopt;
    }
    if (trimmed.find("func ") != std::string::npos || trimmed.rfind("class ", 0) == 0 ||
        trimmed.rfind("struct ", 0) == 0 || trimmed.rfind("interface ", 0) == 0 ||
        trimmed.rfind("if ", 0) == 0 || trimmed.rfind("for ", 0) == 0 || trimmed.rfind("while ", 0) == 0 ||
        trimmed.rfind("match ", 0) == 0 || trimmed.rfind("case ", 0) == 0) {
        return std::nullopt;
    }
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    if (ParseSourceDeclaration(trimmed, name, typeHint, expr) || ParseSourceAssignment(trimmed, name, expr)) {
        return std::nullopt;
    }
    std::string op;
    if (ParseSourceCompoundAssignment(trimmed, name, op, expr)) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<std::string> ExtractSourceInlineBraceResultExpr(const std::string& line)
{
    auto open = line.find('{');
    auto close = line.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return std::nullopt;
    }
    auto body = Trim(line.substr(open + 1, close - open - 1));
    if (body.empty()) {
        return std::nullopt;
    }
    if (auto ret = ExtractSourceReturnExpr(body); ret.has_value() && !ret->empty()) {
        return ret;
    }
    if (auto arrow = body.find("=>"); arrow != std::string::npos) {
        auto expr = TrimSourceMatchArmExpr(body.substr(arrow + 2));
        return expr.empty() ? std::nullopt : std::optional<std::string>(expr);
    }
    return ExtractSourceExpressionResultExpr(body);
}

void AddSourceFunctionSummary(std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    const std::string& name, SourceFunctionSummary summary)
{
    if (name.empty() || (summary.returnExprs.empty() && summary.bodyLines.empty())) {
        return;
    }
    auto it = summaries.find(name);
    if (it == summaries.end() || it->second.params.size() != summary.params.size()) {
        summaries[name] = std::move(summary);
        return;
    }
    it->second.returnExprs.insert(it->second.returnExprs.end(), summary.returnExprs.begin(), summary.returnExprs.end());
    std::sort(it->second.returnExprs.begin(), it->second.returnExprs.end());
    it->second.returnExprs.erase(std::unique(it->second.returnExprs.begin(), it->second.returnExprs.end()),
        it->second.returnExprs.end());
}

std::unordered_map<std::string, SourceFunctionSummary> BuildSourceFunctionSummaries(
    const std::vector<std::string>& lines)
{
    std::unordered_map<std::string, SourceFunctionSummary> summaries;
    for (unsigned lineNo = 1; lineNo <= lines.size(); ++lineNo) {
        auto line = Trim(StripLineComment(lines[lineNo - 1]));
        std::string declared;
        ContestQueryTypeHint declaredType{ContestQueryTypeHint::UNKNOWN};
        std::string declaredExpr;
        if (ParseSourceDeclaration(line, declared, declaredType, declaredExpr)) {
            if (auto lambda = ParseSourceLambdaSummary(declaredExpr); lambda.has_value()) {
                AddSourceFunctionSummary(summaries, declared, std::move(lambda.value()));
            }
        }
        auto funcPos = line.find("func ");
        size_t nameBegin = std::string::npos;
        if (funcPos != std::string::npos) {
            nameBegin = funcPos + 5;
        } else {
            size_t first = 0;
            while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) {
                ++first;
            }
            if (line.compare(first, 4, "main") != 0) {
                continue;
            }
            auto afterMain = first + 4;
            if (afterMain < line.size() && IsIdentifierChar(line[afterMain])) {
                continue;
            }
            nameBegin = first;
        }
        while (nameBegin < line.size() && std::isspace(static_cast<unsigned char>(line[nameBegin]))) {
            ++nameBegin;
        }
        auto nameEnd = nameBegin;
        while (nameEnd < line.size() && IsIdentifierChar(line[nameEnd])) {
            ++nameEnd;
        }
        if (nameBegin == nameEnd) {
            continue;
        }
        auto afterName = SkipSourceGenericParameterList(line, nameEnd);
        auto open = line.find('(', afterName);
        auto close = line.find(')', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos || close <= open) {
            continue;
        }
        SourceFunctionSummary summary;
        summary.startLine = lineNo;
        summary.params = ParseSourceParamNames(line.substr(open + 1, close - open - 1));
        auto arrow = line.find("=>", close);
        if (arrow != std::string::npos) {
            auto ret = TrimSourceMatchArmExpr(line.substr(arrow + 2));
            if (!ret.empty()) {
                summary.returnExprs.emplace_back(std::move(ret));
            }
        }
        if (auto inlineResult = ExtractSourceInlineBraceResultExpr(line); inlineResult.has_value()) {
            summary.returnExprs.emplace_back(inlineResult.value());
        }
        auto fallbackEnd = static_cast<unsigned>(std::min<size_t>(lines.size(), static_cast<size_t>(lineNo) + 64));
        auto end = FindSourceBraceBlockEnd(lines, lineNo).value_or(fallbackEnd);
        summary.endLine = end;
        for (unsigned bodyLine = lineNo + 1; bodyLine < end && bodyLine <= lines.size(); ++bodyLine) {
            summary.bodyLines.emplace_back(lines[bodyLine - 1]);
        }
        std::optional<std::string> lastExpr;
        for (unsigned bodyLine = lineNo; bodyLine <= end && bodyLine <= lines.size(); ++bodyLine) {
            auto returnExpr = ExtractSourceReturnExpr(lines[bodyLine - 1]);
            if (returnExpr.has_value() && !returnExpr->empty()) {
                summary.returnExprs.emplace_back(returnExpr.value());
                continue;
            }
            if (bodyLine > lineNo) {
                auto exprResult = ExtractSourceExpressionResultExpr(lines[bodyLine - 1]);
                if (exprResult.has_value()) {
                    lastExpr = exprResult;
                }
            }
        }
        if (summary.returnExprs.empty() && lastExpr.has_value()) {
            summary.returnExprs.emplace_back(lastExpr.value());
        }
        auto name = line.substr(nameBegin, nameEnd - nameBegin);
        std::sort(summary.returnExprs.begin(), summary.returnExprs.end());
        summary.returnExprs.erase(std::unique(summary.returnExprs.begin(), summary.returnExprs.end()),
            summary.returnExprs.end());
        AddSourceFunctionSummary(summaries, name, std::move(summary));
    }
    return summaries;
}

std::optional<std::vector<int64_t>> NormalizeSourceIntSet(std::vector<int64_t> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty() || values.size() > MAX_CONTEST_EXACT_VALUES) {
        return std::nullopt;
    }
    return values;
}

std::optional<std::vector<int64_t>> EvalSourceIntExprSetWithFunctions(const std::string& expr,
    const std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    size_t depth, bool* usedFunctionSimulation = nullptr);

std::string NormalizeSourceTrailingClosureCall(std::string expr)
{
    expr = Trim(expr);
    if (expr.empty() || expr.back() != '}') {
        return expr;
    }
    int braceDepth = 0;
    size_t braceOpen = std::string::npos;
    for (size_t i = expr.size(); i > 0; --i) {
        auto pos = i - 1;
        if (expr[pos] == '}') {
            ++braceDepth;
        } else if (expr[pos] == '{') {
            --braceDepth;
            if (braceDepth == 0) {
                braceOpen = pos;
                break;
            }
        }
    }
    if (braceOpen == std::string::npos) {
        return expr;
    }
    auto before = Trim(expr.substr(0, braceOpen));
    auto lambda = Trim(expr.substr(braceOpen));
    if (before.empty() || before.back() != ')') {
        return expr;
    }
    auto open = before.find('(');
    if (open == std::string::npos) {
        return expr;
    }
    auto inside = Trim(before.substr(open + 1, before.size() - open - 2));
    return before.substr(0, before.size() - 1) + (inside.empty() ? "" : ", ") + lambda + ")";
}

std::optional<size_t> FindSourceTrailingCallOpen(const std::string& expr)
{
    if (expr.empty() || expr.back() != ')') {
        return std::nullopt;
    }
    std::vector<size_t> stack;
    std::optional<size_t> trailingOpen;
    for (size_t pos = 0; pos < expr.size(); ++pos) {
        if (expr[pos] == '(') {
            if (stack.empty()) {
                trailingOpen = pos;
            }
            stack.emplace_back(pos);
        } else if (expr[pos] == ')') {
            if (stack.empty()) {
                return std::nullopt;
            }
            auto open = stack.back();
            stack.pop_back();
            if (pos == expr.size() - 1 && stack.empty()) {
                return open;
            }
        }
    }
    return trailingOpen;
}

void SetSourceValueInNearestScope(std::vector<SourceScope>& scopes, const std::string& name, const SourceExactValue& value)
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto existing = it->find(name);
        if (existing != it->end()) {
            existing->second = value;
            return;
        }
    }
    if (scopes.empty()) {
        scopes.emplace_back();
    }
    scopes.back()[name] = value;
}

void EraseSourceValueFromScopes(std::vector<SourceScope>& scopes, const std::string& name)
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto existing = it->find(name);
        if (existing != it->end()) {
            it->erase(existing);
            return;
        }
    }
}

std::vector<SourceScope> BuildSourceGlobalScopes(const std::vector<std::string>& lines)
{
    std::vector<SourceScope> scopes(1);
    int braceDepth = 0;
    for (unsigned lineNo = 1; lineNo <= lines.size(); ++lineNo) {
        auto line = lines[lineNo - 1];
        auto stripped = StripLineComment(line);
        const bool isGlobalLike = braceDepth == 0 || stripped.find("static") != std::string::npos;
        if (isGlobalLike) {
            std::string name;
            ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
            std::string expr;
            if (ParseSourceDeclaration(line, name, typeHint, expr)) {
                SourceExactValue exact;
                auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
                if (TryEvalSourceExactValue(expr, scopes, typeHint, exact, overflowStrategy)) {
                    scopes.front()[name] = exact;
                }
            } else if (ParseSourceAssignment(line, name, expr)) {
                SourceExactValue exact;
                auto old = LookupSourceValue(scopes, name);
                auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
                auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
                if (TryEvalSourceExactValue(expr, scopes, hint, exact, overflowStrategy)) {
                    scopes.front()[name] = exact;
                }
            } else {
                std::string compoundName;
                std::string op;
                std::string compoundExpr;
                if (ParseSourceCompoundAssignment(line, compoundName, op, compoundExpr)) {
                    auto value = EvalSourceCompoundAssignment(scopes, compoundName, op, compoundExpr);
                    if (value.has_value()) {
                        SourceExactValue exact;
                        exact.typeHint = ContestQueryTypeHint::INT64;
                        exact.intValue = value.value();
                        scopes.front()[compoundName] = exact;
                    }
                }
            }
        }
        for (auto c : stripped) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}' && braceDepth > 0) {
                --braceDepth;
            }
        }
    }
    return scopes;
}

std::optional<std::string> ParseSourceControlCondition(const std::string& line, const std::string& keyword)
{
    auto trimmed = Trim(StripLineComment(line));
    if (!SourceLineStartsWithKeyword(trimmed, keyword)) {
        return std::nullopt;
    }
    auto open = trimmed.find('(');
    if (open == std::string::npos) {
        return std::nullopt;
    }
    auto close = FindMatchingSourceParen(trimmed, open);
    if (!close.has_value() || close.value() <= open) {
        return std::nullopt;
    }
    return Trim(trimmed.substr(open + 1, close.value() - open - 1));
}

std::optional<size_t> FindSourceVectorBlockEnd(const std::vector<std::string>& lines, size_t header)
{
    if (header >= lines.size()) {
        return std::nullopt;
    }
    int depth = 0;
    bool sawOpen = false;
    for (size_t line = header; line < lines.size(); ++line) {
        auto raw = StripLineComment(lines[line]);
        for (auto c : raw) {
            if (c == '{') {
                ++depth;
                sawOpen = true;
            } else if (c == '}' && sawOpen) {
                --depth;
                if (depth == 0) {
                    return line;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<SourceExactValue> EvalSourceExactForSimulation(const std::string& expr,
    std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    ContestQueryTypeHint preferredHint, size_t depth)
{
    SourceExactValue exact;
    if (TryEvalSourceExactValue(expr, scopes, preferredHint, exact)) {
        return exact;
    }
    if (preferredHint == ContestQueryTypeHint::BOOL) {
        return std::nullopt;
    }
    auto values = EvalSourceIntExprSetWithFunctions(expr, scopes, summaries, depth + 1);
    if (!values.has_value() || values->size() != 1) {
        return std::nullopt;
    }
    exact.typeHint = IsSourceIntegerTypeHint(preferredHint) ? preferredHint : ContestQueryTypeHint::INT64;
    exact.intValue = values->front();
    return exact;
}

struct SourceSimulationTarget {
    unsigned sourceLine{0};
    std::string variableName;
};

struct SourceSimulationResult {
    bool failed{false};
    std::optional<int64_t> returnValue;
    std::vector<int64_t> observedValues;
};

SourceSimulationResult ExecuteSourceSummaryBlock(const std::vector<std::string>& lines, size_t begin, size_t end,
    std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    size_t depth, size_t& budget, unsigned baseSourceLine, const SourceSimulationTarget* target = nullptr)
{
    if (depth > 8) {
        return SourceSimulationResult{true, std::nullopt};
    }
    if (lines.empty() || begin > end || begin >= lines.size()) {
        return SourceSimulationResult{false, std::nullopt};
    }
    end = std::min(end, lines.size() - 1);
    SourceSimulationResult result;
    auto simulateDirectCall = [&](const std::string& expr) -> std::optional<SourceSimulationResult> {
        auto normalized = StripSourceEnclosingParens(NormalizeSourceTrailingClosureCall(expr));
        if (!normalized.empty() && normalized.back() == ';') {
            normalized = Trim(normalized.substr(0, normalized.size() - 1));
        }
        auto open = FindSourceTrailingCallOpen(normalized);
        if (!open.has_value()) {
            return std::nullopt;
        }
        auto callee = StripSourceGenericSuffix(normalized.substr(0, open.value()));
        auto summaryIt = summaries.find(callee);
        if (summaryIt == summaries.end() || summaryIt->second.bodyLines.empty()) {
            return std::nullopt;
        }
        auto args = SplitSourceTopLevelCommaList(
            normalized.substr(open.value() + 1, normalized.size() - open.value() - 2));
        if (args.size() != summaryIt->second.params.size()) {
            return std::nullopt;
        }
        std::vector<SourceScope> calleeScopes(1);
        if (!scopes.empty()) {
            calleeScopes.front() = scopes.front();
        }
        calleeScopes.emplace_back();
        for (size_t i = 0; i < args.size(); ++i) {
            auto exact = EvalSourceExactForSimulation(args[i], scopes, summaries, ContestQueryTypeHint::INT64, depth + 1);
            if (!exact.has_value() || !IsSourceIntegerTypeHint(exact->typeHint)) {
                return std::nullopt;
            }
            calleeScopes.back()[summaryIt->second.params[i]] = exact.value();
        }
        auto callResult = ExecuteSourceSummaryBlock(summaryIt->second.bodyLines, 0, summaryIt->second.bodyLines.size() - 1,
            calleeScopes, summaries, depth + 1, budget, summaryIt->second.startLine + 1, target);
        if (!callResult.failed && !scopes.empty() && !calleeScopes.empty()) {
            scopes.front() = calleeScopes.front();
        }
        return callResult;
    };
    auto observeTargetWrite = [&](unsigned currentLine, const std::string& name,
                                  const std::optional<SourceExactValue>& exact) {
        if (target == nullptr || target->sourceLine != currentLine || name != target->variableName ||
            !exact.has_value() || !IsSourceIntegerTypeHint(exact->typeHint)) {
            return;
        }
        result.observedValues.emplace_back(exact->intValue);
    };
    auto observeTargetCurrent = [&](unsigned currentLine) {
        if (target == nullptr || target->sourceLine != currentLine) {
            return;
        }
        auto value = LookupSourceValue(scopes, target->variableName);
        if (value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
            result.observedValues.emplace_back(value->intValue);
        }
    };
    for (size_t index = begin; index <= end; ++index) {
        if (budget == 0) {
            result.failed = true;
            return result;
        }
        --budget;
        if (target != nullptr && target->sourceLine == baseSourceLine + index) {
            auto value = LookupSourceValue(scopes, target->variableName);
            if (value != nullptr && IsSourceIntegerTypeHint(value->typeHint)) {
                result.observedValues.emplace_back(value->intValue);
            }
        }
        auto trimmed = Trim(StripLineComment(lines[index]));
        if (trimmed.empty() || trimmed == "{" || trimmed == "}") {
            continue;
        }
        if (trimmed == "break" || trimmed == "break;" || trimmed == "continue" || trimmed == "continue;") {
            result.failed = true;
            return result;
        }
        if (SourceLineStartsWithKeyword(trimmed, "for")) {
            auto forRange = ParseSourceForInRangeHeaderWithScopes(trimmed, scopes);
            auto blockEnd = FindSourceVectorBlockEnd(lines, index);
            if (!forRange.has_value() || !blockEnd.has_value() || blockEnd.value() <= index) {
                result.failed = true;
                return result;
            }
            auto bounds = GetSourceForRangeBounds(forRange.value());
            if (!bounds.has_value()) {
                result.failed = true;
                return result;
            }
            size_t iterations = 0;
            for (int64_t iterValue = bounds->first; iterValue <= bounds->second; ++iterValue) {
                if (++iterations > 1024) {
                    result.failed = true;
                    return result;
                }
                scopes.emplace_back();
                SourceExactValue exact;
                exact.typeHint = ContestQueryTypeHint::INT64;
                exact.intValue = iterValue;
                scopes.back()[forRange->variable] = exact;
                observeTargetCurrent(baseSourceLine + static_cast<unsigned>(index));
                auto inner = ExecuteSourceSummaryBlock(
                    lines, index + 1, blockEnd.value() - 1, scopes, summaries, depth + 1, budget, baseSourceLine, target);
                if (scopes.size() > 1) {
                    scopes.pop_back();
                }
                result.observedValues.insert(
                    result.observedValues.end(), inner.observedValues.begin(), inner.observedValues.end());
                if (inner.failed) {
                    result.failed = true;
                    return result;
                }
                if (inner.returnValue.has_value()) {
                    result.returnValue = inner.returnValue;
                    return result;
                }
                if (iterValue == bounds->second) {
                    break;
                }
            }
            if (target != nullptr && target->sourceLine == baseSourceLine + static_cast<unsigned>(index) &&
                target->variableName == forRange->variable && bounds->second != std::numeric_limits<int64_t>::max()) {
                result.observedValues.emplace_back(bounds->second + 1);
            }
            index = blockEnd.value();
            continue;
        }
        if (SourceLineStartsWithKeyword(trimmed, "while")) {
            auto condition = ParseSourceControlCondition(trimmed, "while");
            auto blockEnd = FindSourceVectorBlockEnd(lines, index);
            if (!condition.has_value() || !blockEnd.has_value() || blockEnd.value() <= index) {
                result.failed = true;
                return result;
            }
            size_t iterations = 0;
            while (true) {
                observeTargetCurrent(baseSourceLine + static_cast<unsigned>(index));
                auto conditionValue = EvalSourceBoolExpr(condition.value(), scopes);
                if (!conditionValue.has_value()) {
                    result.failed = true;
                    return result;
                }
                if (!conditionValue.value()) {
                    break;
                }
                if (++iterations > 1024) {
                    result.failed = true;
                    return result;
                }
                scopes.emplace_back();
                auto inner = ExecuteSourceSummaryBlock(
                    lines, index + 1, blockEnd.value() - 1, scopes, summaries, depth + 1, budget, baseSourceLine, target);
                if (scopes.size() > 1) {
                    scopes.pop_back();
                }
                result.observedValues.insert(
                    result.observedValues.end(), inner.observedValues.begin(), inner.observedValues.end());
                if (inner.failed) {
                    result.failed = true;
                    return result;
                }
                if (inner.returnValue.has_value()) {
                    result.returnValue = inner.returnValue;
                    return result;
                }
            }
            index = blockEnd.value();
            continue;
        }
        if (SourceLineStartsWithKeyword(trimmed, "if")) {
            auto condition = ParseSourceControlCondition(trimmed, "if");
            auto blockEnd = FindSourceVectorBlockEnd(lines, index);
            if (!condition.has_value() || !blockEnd.has_value() || blockEnd.value() <= index) {
                result.failed = true;
                return result;
            }
            auto conditionValue = EvalSourceBoolExpr(condition.value(), scopes);
            if (!conditionValue.has_value()) {
                result.failed = true;
                return result;
            }
            if (conditionValue.value()) {
                scopes.emplace_back();
                auto inner = ExecuteSourceSummaryBlock(
                    lines, index + 1, blockEnd.value() - 1, scopes, summaries, depth + 1, budget, baseSourceLine, target);
                if (scopes.size() > 1) {
                    scopes.pop_back();
                }
                result.observedValues.insert(
                    result.observedValues.end(), inner.observedValues.begin(), inner.observedValues.end());
                if (inner.failed) {
                    result.failed = true;
                    return result;
                }
                if (inner.returnValue.has_value()) {
                    result.returnValue = inner.returnValue;
                    return result;
                }
            }
            index = blockEnd.value();
            continue;
        }
        if (auto direct = simulateDirectCall(trimmed); direct.has_value()) {
            result.observedValues.insert(
                result.observedValues.end(), direct->observedValues.begin(), direct->observedValues.end());
            if (direct->failed) {
                result.failed = true;
                return result;
            }
            continue;
        }
        if (auto ret = ExtractSourceReturnExpr(trimmed); ret.has_value() && !ret->empty()) {
            if (auto direct = simulateDirectCall(ret.value()); direct.has_value()) {
                result.observedValues.insert(
                    result.observedValues.end(), direct->observedValues.begin(), direct->observedValues.end());
                if (direct->failed || !direct->returnValue.has_value()) {
                    result.failed = true;
                    return result;
                }
                result.returnValue = direct->returnValue;
            } else {
                auto exact = EvalSourceExactForSimulation(
                    ret.value(), scopes, summaries, ContestQueryTypeHint::UNKNOWN, depth + 1);
                if (!exact.has_value() || !IsSourceIntegerTypeHint(exact->typeHint)) {
                    result.failed = true;
                    return result;
                }
                result.returnValue = exact->intValue;
            }
            return result;
        }
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(trimmed, name, typeHint, expr)) {
            std::optional<SourceExactValue> exact;
            if (auto direct = simulateDirectCall(expr); direct.has_value()) {
                result.observedValues.insert(
                    result.observedValues.end(), direct->observedValues.begin(), direct->observedValues.end());
                if (direct->failed) {
                    result.failed = true;
                    return result;
                }
                if (direct->returnValue.has_value()) {
                    SourceExactValue directValue;
                    directValue.typeHint = IsSourceIntegerTypeHint(typeHint) ? typeHint : ContestQueryTypeHint::INT64;
                    directValue.intValue = direct->returnValue.value();
                    exact = directValue;
                }
            } else {
                exact = EvalSourceExactForSimulation(expr, scopes, summaries, typeHint, depth + 1);
            }
            if (exact.has_value()) {
                if (scopes.empty()) {
                    scopes.emplace_back();
                }
                scopes.back()[name] = exact.value();
            }
            observeTargetWrite(baseSourceLine + static_cast<unsigned>(index), name, exact);
            continue;
        }
        if (ParseSourceAssignment(trimmed, name, expr)) {
            auto old = LookupSourceValue(scopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            std::optional<SourceExactValue> exact;
            if (auto direct = simulateDirectCall(expr); direct.has_value()) {
                result.observedValues.insert(
                    result.observedValues.end(), direct->observedValues.begin(), direct->observedValues.end());
                if (direct->failed) {
                    result.failed = true;
                    return result;
                }
                if (direct->returnValue.has_value()) {
                    SourceExactValue directValue;
                    directValue.typeHint = IsSourceIntegerTypeHint(hint) ? hint : ContestQueryTypeHint::INT64;
                    directValue.intValue = direct->returnValue.value();
                    exact = directValue;
                }
            } else {
                exact = EvalSourceExactForSimulation(expr, scopes, summaries, hint, depth + 1);
            }
            if (exact.has_value()) {
                SetSourceValueInNearestScope(scopes, name, exact.value());
            } else {
                EraseSourceValueFromScopes(scopes, name);
            }
            observeTargetWrite(baseSourceLine + static_cast<unsigned>(index), name, exact);
            continue;
        }
        std::string op;
        if (ParseSourceCompoundAssignment(trimmed, name, op, expr)) {
            std::optional<SourceExactValue> exact;
            auto value = EvalSourceCompoundAssignment(scopes, name, op, expr);
            if (value.has_value()) {
                auto old = LookupSourceValue(scopes, name);
                SourceExactValue current;
                current.typeHint = old == nullptr ? ContestQueryTypeHint::INT64 : old->typeHint;
                current.intValue = value.value();
                exact = current;
                SetSourceValueInNearestScope(scopes, name, current);
            } else {
                EraseSourceValueFromScopes(scopes, name);
            }
            observeTargetWrite(baseSourceLine + static_cast<unsigned>(index), name, exact);
            continue;
        }
    }
    return result;
}

std::optional<int64_t> SimulateSourceFunctionSummary(const SourceFunctionSummary& summary,
    const std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    size_t depth)
{
    if (summary.bodyLines.empty()) {
        return std::nullopt;
    }
    auto localScopes = scopes;
    localScopes.emplace_back();
    size_t budget = 20000;
    auto result = ExecuteSourceSummaryBlock(
        summary.bodyLines, 0, summary.bodyLines.size() - 1, localScopes, summaries, depth + 1, budget,
        summary.startLine + 1);
    if (result.failed) {
        return std::nullopt;
    }
    return result.returnValue;
}

std::optional<std::vector<int64_t>> SimulateSourceFunctionSummaryAtLine(const SourceFunctionSummary& summary,
    const std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    unsigned sourceLine, const std::string& variableName, size_t depth)
{
    if (summary.bodyLines.empty() || summary.startLine == 0 || summary.endLine == 0 ||
        sourceLine == 0 || !IsSourceIdentifierName(variableName)) {
        return std::nullopt;
    }
    auto localScopes = scopes;
    localScopes.emplace_back();
    SourceSimulationTarget target{sourceLine, variableName};
    size_t budget = 20000;
    auto result = ExecuteSourceSummaryBlock(
        summary.bodyLines, 0, summary.bodyLines.size() - 1, localScopes, summaries, depth + 1, budget,
        summary.startLine + 1, &target);
    if (result.failed || result.observedValues.empty()) {
        return std::nullopt;
    }
    return NormalizeSourceIntSet(std::move(result.observedValues));
}

std::optional<std::vector<int64_t>> EvalSourceIntExprSetWithFunctions(const std::string& expr,
    const std::vector<SourceScope>& scopes, const std::unordered_map<std::string, SourceFunctionSummary>& summaries,
    size_t depth, bool* usedFunctionSimulation)
{
    if (depth > 8) {
        return std::nullopt;
    }
    if (auto value = EvalSourceIntExpr(expr, scopes); value.has_value()) {
        return std::vector<int64_t>{value.value()};
    }
    auto trimmed = StripSourceEnclosingParens(NormalizeSourceTrailingClosureCall(expr));
    const std::vector<std::vector<std::string>> precedenceGroups{{"|"}, {"^"}, {"&"}, {"<<", ">>"}, {"+", "-"},
        {"*", "/", "%"}, {"**"}};
    for (const auto& group : precedenceGroups) {
        for (const auto& token : group) {
            auto pos = FindSourceBinaryToken(trimmed, token);
            if (!pos.has_value()) {
                continue;
            }
            auto lhsValues = EvalSourceIntExprSetWithFunctions(
                trimmed.substr(0, pos.value()), scopes, summaries, depth + 1, usedFunctionSimulation);
            auto rhsValues = EvalSourceIntExprSetWithFunctions(
                trimmed.substr(pos.value() + token.size()), scopes, summaries, depth + 1, usedFunctionSimulation);
            if (!lhsValues.has_value() || !rhsValues.has_value()) {
                return std::nullopt;
            }
            std::vector<int64_t> values;
            for (auto lhs : lhsValues.value()) {
                for (auto rhs : rhsValues.value()) {
                    auto result = EvalSourceBinaryIntOp(lhs, rhs, token);
                    if (!result.has_value()) {
                        return std::nullopt;
                    }
                    values.emplace_back(result.value());
                    if (values.size() > MAX_CONTEST_EXACT_VALUES) {
                        return std::nullopt;
                    }
                }
            }
            return NormalizeSourceIntSet(std::move(values));
        }
    }
    auto open = FindSourceTrailingCallOpen(trimmed);
    if (!open.has_value()) {
        return std::nullopt;
    }
    auto callee = StripSourceGenericSuffix(trimmed.substr(0, open.value()));
    auto it = summaries.find(callee);
    if (it == summaries.end()) {
        return std::nullopt;
    }
    auto args = SplitSourceTopLevelCommaList(trimmed.substr(open.value() + 1, trimmed.size() - open.value() - 2));
    if (args.size() != it->second.params.size()) {
        return std::nullopt;
    }
    std::vector<std::vector<int64_t>> argValues(args.size());
    std::vector<bool> isLambdaArg(args.size(), false);
    auto localSummaries = summaries;
    for (size_t i = 0; i < args.size(); ++i) {
        if (auto lambda = ParseSourceLambdaSummary(args[i]); lambda.has_value()) {
            localSummaries[it->second.params[i]] = std::move(lambda.value());
            isLambdaArg[i] = true;
            continue;
        }
        auto argCallee = StripSourceGenericSuffix(args[i]);
        if (auto summaryIt = localSummaries.find(argCallee); summaryIt != localSummaries.end()) {
            localSummaries[it->second.params[i]] = summaryIt->second;
            isLambdaArg[i] = true;
            continue;
        }
        auto values = EvalSourceIntExprSetWithFunctions(args[i], scopes, localSummaries, depth + 1,
            usedFunctionSimulation);
        if (!values.has_value() || values->empty()) {
            return std::nullopt;
        }
        argValues[i] = std::move(values.value());
    }
    std::vector<int64_t> results;
    std::vector<size_t> valueArgIndices;
    for (size_t i = 0; i < argValues.size(); ++i) {
        if (!isLambdaArg[i]) {
            valueArgIndices.emplace_back(i);
        }
    }
    std::vector<size_t> indices(valueArgIndices.size(), 0);
    while (true) {
        std::vector<SourceScope> calleeScopes(1);
        if (!scopes.empty()) {
            calleeScopes.front() = scopes.front();
        }
        for (size_t indexPos = 0; indexPos < valueArgIndices.size(); ++indexPos) {
            auto argIndex = valueArgIndices[indexPos];
            SourceExactValue exact;
            exact.typeHint = ContestQueryTypeHint::INT64;
            exact.intValue = argValues[argIndex][indices[indexPos]];
            calleeScopes.back()[it->second.params[argIndex]] = exact;
        }
        if (auto simulated = SimulateSourceFunctionSummary(it->second, calleeScopes, localSummaries, depth + 1);
            simulated.has_value()) {
            if (usedFunctionSimulation != nullptr) {
                *usedFunctionSimulation = true;
            }
            results.emplace_back(simulated.value());
            if (results.size() > MAX_CONTEST_EXACT_VALUES) {
                return std::nullopt;
            }
        } else {
        for (const auto& ret : it->second.returnExprs) {
            auto values = EvalSourceIntExprSetWithFunctions(ret, calleeScopes, localSummaries, depth + 1,
                usedFunctionSimulation);
            if (!values.has_value()) {
                continue;
            }
            results.insert(results.end(), values->begin(), values->end());
            if (results.size() > MAX_CONTEST_EXACT_VALUES) {
                return std::nullopt;
            }
        }
        }
        if (indices.empty()) {
            break;
        }
        size_t pos = indices.size();
        while (pos > 0) {
            --pos;
            ++indices[pos];
            auto argIndex = valueArgIndices[pos];
            if (indices[pos] < argValues[argIndex].size()) {
                break;
            }
            indices[pos] = 0;
        }
        if (pos == 0 && indices[0] == 0) {
            break;
        }
    }
    return NormalizeSourceIntSet(std::move(results));
}

bool SourceExprContainsIdentifier(const std::string& expr, const std::string& name)
{
    size_t pos = 0;
    while ((pos = expr.find(name, pos)) != std::string::npos) {
        auto before = pos == 0 ? '\0' : expr[pos - 1];
        auto afterPos = pos + name.size();
        auto after = afterPos >= expr.size() ? '\0' : expr[afterPos];
        if (!IsIdentifierChar(before) && !IsIdentifierChar(after)) {
            return true;
        }
        pos = afterPos;
    }
    return false;
}

std::optional<int64_t> EvalSourceInlineSpawnExpr(
    const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = Trim(StripLineComment(expr));
    auto spawnPos = trimmed.find("spawn");
    if (spawnPos == std::string::npos) {
        return std::nullopt;
    }
    auto open = trimmed.find('{', spawnPos);
    if (open == std::string::npos) {
        return std::nullopt;
    }
    int braceDepth = 0;
    size_t close = std::string::npos;
    for (size_t pos = open; pos < trimmed.size(); ++pos) {
        if (trimmed[pos] == '{') {
            ++braceDepth;
        } else if (trimmed[pos] == '}') {
            --braceDepth;
            if (braceDepth == 0) {
                close = pos;
                break;
            }
        }
    }
    if (close == std::string::npos || close <= open) {
        return std::nullopt;
    }
    auto body = Trim(trimmed.substr(open + 1, close - open - 1));
    auto localScopes = scopes;
    if (localScopes.empty()) {
        localScopes.emplace_back();
    }
    std::optional<int64_t> lastValue;
    size_t begin = 0;
    int stmtDepth = 0;
    for (size_t pos = 0; pos <= body.size(); ++pos) {
        if (pos < body.size()) {
            if (body[pos] == '{' || body[pos] == '(' || body[pos] == '[') {
                ++stmtDepth;
            } else if ((body[pos] == '}' || body[pos] == ')' || body[pos] == ']') && stmtDepth > 0) {
                --stmtDepth;
            }
        }
        if (pos < body.size() && (body[pos] != ';' || stmtDepth != 0)) {
            continue;
        }
        auto stmt = Trim(body.substr(begin, pos - begin));
        begin = pos + 1;
        if (stmt.empty()) {
            continue;
        }
        if (stmt.rfind("return ", 0) == 0) {
            return EvalSourceIntExpr(Trim(stmt.substr(7)), localScopes);
        }
        if (auto arrow = stmt.find("=>"); arrow != std::string::npos) {
            stmt = TrimSourceMatchArmExpr(stmt.substr(arrow + 2));
        }
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string rhs;
        if (ParseSourceDeclaration(stmt, name, typeHint, rhs)) {
            SourceExactValue exact;
            if (TryEvalSourceExactValue(rhs, localScopes, typeHint, exact)) {
                localScopes.back()[name] = exact;
            }
            continue;
        }
        if (ParseSourceAssignment(stmt, name, rhs)) {
            SourceExactValue exact;
            auto old = LookupSourceValue(localScopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            if (TryEvalSourceExactValue(rhs, localScopes, hint, exact)) {
                localScopes.back()[name] = exact;
            }
            continue;
        }
        if (auto value = EvalSourceIntExpr(stmt, localScopes); value.has_value()) {
            lastValue = value;
        }
    }
    return lastValue;
}

std::optional<int64_t> EvalSourceSpawnExprBlock(
    const std::vector<std::string>& lines, unsigned startLine, const std::vector<SourceScope>& scopes)
{
    if (startLine == 0 || startLine > lines.size()) {
        return std::nullopt;
    }
    auto first = StripLineComment(lines[startLine - 1]);
    if (first.find("spawn") == std::string::npos) {
        return std::nullopt;
    }
    if (auto inlineValue = EvalSourceInlineSpawnExpr(first, scopes); inlineValue.has_value()) {
        return inlineValue;
    }
    auto limit = std::min<unsigned>(static_cast<unsigned>(lines.size()), startLine + 64);
    int depth = 0;
    bool sawBrace = false;
    auto localScopes = scopes;
    if (localScopes.empty()) {
        localScopes.emplace_back();
    }
    std::optional<int64_t> lastValue;
    for (unsigned lineNo = startLine; lineNo <= limit; ++lineNo) {
        auto raw = StripLineComment(lines[lineNo - 1]);
        auto trimmed = Trim(raw);
        if (auto ret = ExtractSourceReturnExpr(trimmed); ret.has_value()) {
            if (auto value = EvalSourceIntExpr(ret.value(), localScopes); value.has_value()) {
                return value;
            }
        }
        auto arrow = trimmed.find("=>");
        if (arrow != std::string::npos) {
            auto expr = TrimSourceMatchArmExpr(trimmed.substr(arrow + 2));
            if (auto close = expr.find('}'); close != std::string::npos) {
                expr = Trim(expr.substr(0, close));
            }
            if (auto value = EvalSourceIntExpr(expr, localScopes); value.has_value()) {
                return value;
            }
        }
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(raw, name, typeHint, expr) && expr.find("spawn") == std::string::npos) {
            SourceExactValue exact;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            if (TryEvalSourceExactValue(expr, localScopes, typeHint, exact, overflowStrategy)) {
                localScopes.back()[name] = exact;
            }
        } else if (ParseSourceAssignment(raw, name, expr)) {
            SourceExactValue exact;
            auto old = LookupSourceValue(localScopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            if (TryEvalSourceExactValue(expr, localScopes, hint, exact, overflowStrategy)) {
                localScopes.back()[name] = exact;
            }
        } else if (trimmed.find("spawn") == std::string::npos &&
            trimmed.rfind("try", 0) != 0 && trimmed.rfind("catch", 0) != 0 &&
            trimmed.rfind("finally", 0) != 0) {
            auto exprLine = TrimSourceMatchArmExpr(trimmed);
            if (auto close = exprLine.find('}'); close != std::string::npos) {
                exprLine = Trim(exprLine.substr(0, close));
            }
            if (auto semi = exprLine.rfind(';'); semi != std::string::npos) {
                exprLine = Trim(exprLine.substr(semi + 1));
            }
            if (!exprLine.empty() && exprLine != "{" && exprLine != "}") {
                if (auto value = EvalSourceIntExpr(exprLine, localScopes); value.has_value()) {
                    lastValue = value;
                }
            }
        }
        for (auto c : raw) {
            if (c == '{') {
                ++depth;
                sawBrace = true;
                localScopes.emplace_back();
            } else if (c == '}' && depth > 0) {
                --depth;
                if (localScopes.size() > 1) {
                    localScopes.pop_back();
                }
            }
        }
        if (sawBrace && depth == 0 && lineNo > startLine) {
            break;
        }
    }
    return lastValue;
}

std::vector<SourceScope> BuildSourceScopesBeforeLine(const std::vector<std::string>& lines, unsigned beforeLine)
{
    std::vector<SourceScope> scopes(1);
    for (unsigned lineNo = 1; lineNo < beforeLine && lineNo <= lines.size(); ++lineNo) {
        auto line = lines[lineNo - 1];
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(line, name, typeHint, expr)) {
            SourceExactValue exact;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            bool hasExact = TryEvalSourceExactValue(expr, scopes, typeHint, exact, overflowStrategy);
            if (!hasExact && expr.find("spawn") != std::string::npos) {
                if (auto value = EvalSourceSpawnExprBlock(lines, lineNo, scopes); value.has_value()) {
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    hasExact = true;
                }
            }
            if (hasExact) {
                scopes.back()[name] = exact;
                if (scopes.size() == 1 || StripLineComment(line).find("static") != std::string::npos) {
                    scopes.front()[name] = exact;
                }
            }
        } else if (ParseSourceAssignment(line, name, expr)) {
            SourceExactValue exact;
            auto old = LookupSourceValue(scopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            bool hasExact = TryEvalSourceExactValue(expr, scopes, hint, exact, overflowStrategy);
            if (!hasExact && expr.find("spawn") != std::string::npos) {
                if (auto value = EvalSourceSpawnExprBlock(lines, lineNo, scopes); value.has_value()) {
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    hasExact = true;
                }
            }
            if (hasExact) {
                scopes.back()[name] = exact;
                if (scopes.size() == 1 || StripLineComment(line).find("static") != std::string::npos) {
                    scopes.front()[name] = exact;
                }
            }
        } else {
            std::string op;
            if (ParseSourceCompoundAssignment(line, name, op, expr)) {
                auto value = EvalSourceCompoundAssignment(scopes, name, op, expr);
                if (value.has_value()) {
                    SourceExactValue exact;
                    exact.typeHint = ContestQueryTypeHint::INT64;
                    exact.intValue = value.value();
                    scopes.back()[name] = exact;
                    if (scopes.size() == 1 || StripLineComment(line).find("static") != std::string::npos) {
                        scopes.front()[name] = exact;
                    }
                }
            }
        }
        for (auto c : line) {
            if (c == '{') {
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                scopes.pop_back();
            }
        }
    }
    return scopes;
}

void CollectSourceFunctionContextValuesFromExpr(const std::string& expr, const std::vector<SourceScope>& scopes,
    const std::unordered_map<std::string, SourceFunctionSummary>& summaries, const ContestQuery& query,
    std::vector<int64_t>& values)
{
    auto trimmed = StripSourceEnclosingParens(NormalizeSourceTrailingClosureCall(expr));
    auto open = FindSourceTrailingCallOpen(trimmed);
    if (!open.has_value()) {
        return;
    }
    auto callee = StripSourceGenericSuffix(trimmed.substr(0, open.value()));
    auto summaryIt = summaries.find(callee);
    if (summaryIt == summaries.end()) {
        return;
    }
    auto args = SplitSourceTopLevelCommaList(trimmed.substr(open.value() + 1, trimmed.size() - open.value() - 2));
    if (args.size() != summaryIt->second.params.size()) {
        return;
    }
    std::vector<std::vector<int64_t>> argValues(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        bool usedFunctionSimulation = false;
        auto current = EvalSourceIntExprSetWithFunctions(args[i], scopes, summaries, 0, &usedFunctionSimulation);
        if (!current.has_value() || current->empty()) {
            return;
        }
        argValues[i] = std::move(current.value());
    }

    std::vector<size_t> indices(argValues.size(), 0);
    while (true) {
        std::vector<SourceScope> calleeScopes(1);
        if (!scopes.empty()) {
            calleeScopes.front() = scopes.front();
        }
        for (size_t i = 0; i < argValues.size(); ++i) {
            SourceExactValue exact;
            exact.typeHint = ContestQueryTypeHint::INT64;
            exact.intValue = argValues[i][indices[i]];
            calleeScopes.back()[summaryIt->second.params[i]] = exact;
        }
        auto observed = SimulateSourceFunctionSummaryAtLine(
            summaryIt->second, calleeScopes, summaries, query.line, query.variableName, 0);
        if (observed.has_value()) {
            values.insert(values.end(), observed->begin(), observed->end());
            if (values.size() > MAX_CONTEST_EXACT_VALUES) {
                return;
            }
        }
        if (indices.empty()) {
            break;
        }
        size_t pos = indices.size();
        while (pos > 0) {
            --pos;
            ++indices[pos];
            if (indices[pos] < argValues[pos].size()) {
                break;
            }
            indices[pos] = 0;
        }
        if (pos == 0 && indices[0] == 0) {
            break;
        }
    }
}

bool SourceSummaryPrefixUnsupportedForLocalSimulation(const SourceFunctionSummary& summary, unsigned queryLine)
{
    if (summary.startLine == 0 || queryLine <= summary.startLine) {
        return true;
    }
    for (size_t index = 0; index < summary.bodyLines.size(); ++index) {
        auto bodyLine = summary.startLine + 1 + static_cast<unsigned>(index);
        if (bodyLine >= queryLine) {
            break;
        }
        auto trimmed = Trim(StripLineComment(summary.bodyLines[index]));
        if (trimmed.empty()) {
            continue;
        }
        if (SourceLineStartsWithKeyword(trimmed, "match") || SourceLineStartsWithKeyword(trimmed, "try") ||
            SourceLineStartsWithKeyword(trimmed, "catch") || SourceLineStartsWithKeyword(trimmed, "finally") ||
            trimmed.find("else") != std::string::npos || trimmed.find(" spawn") != std::string::npos ||
            trimmed.rfind("spawn", 0) == 0 ||
            trimmed == "break" || trimmed == "break;" || trimmed == "continue" || trimmed == "continue;") {
            return true;
        }
    }
    return false;
}

bool SourceSummaryPrefixHasLoopForLocalSimulation(const SourceFunctionSummary& summary, unsigned queryLine)
{
    if (summary.startLine == 0 || queryLine <= summary.startLine) {
        return false;
    }
    for (size_t index = 0; index < summary.bodyLines.size(); ++index) {
        auto bodyLine = summary.startLine + 1 + static_cast<unsigned>(index);
        if (bodyLine >= queryLine) {
            break;
        }
        auto trimmed = Trim(StripLineComment(summary.bodyLines[index]));
        if (SourceLineStartsWithKeyword(trimmed, "while") || SourceLineStartsWithKeyword(trimmed, "for")) {
            return true;
        }
    }
    return false;
}

void InferSourceLocalSimulationFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    if (query.hasLineSensitiveLoopFallback) {
        return;
    }
    if (SourceLineDeclaresMutableVariableName(lines[query.line - 1], query.variableName)) {
        auto loop = FindInnermostSourceLoop(lines, query.line);
        auto end = loop.has_value() && loop->end > query.line ? loop->end - 1 : static_cast<unsigned>(lines.size());
        if (query.line < end && CountSourceAssignments(lines, query.line + 1, end, query.variableName) > 0) {
            return;
        }
    }
    auto summaries = BuildSourceFunctionSummaries(lines);
    if (summaries.empty()) {
        return;
    }
    for (const auto& [name, summary] : summaries) {
        (void)name;
        if (!summary.params.empty() || summary.startLine == 0 || summary.endLine == 0 ||
            !(summary.startLine < query.line && query.line < summary.endLine)) {
            continue;
        }
        if (SourceSummaryPrefixUnsupportedForLocalSimulation(summary, query.line)) {
            continue;
        }
        if (!SourceSummaryPrefixHasLoopForLocalSimulation(summary, query.line)) {
            continue;
        }
        auto scopes = BuildSourceGlobalScopes(lines);
        auto observed = SimulateSourceFunctionSummaryAtLine(summary, scopes, summaries, query.line, query.variableName, 0);
        if (!observed.has_value()) {
            continue;
        }
        SetSourceIntSetFallback(query, std::move(observed.value()));
        query.preferSourceFallback = query.hasSourceFallback;
        query.sourceFallbackMayBeLoopNarrow = query.hasSourceFallback;
        return;
    }
}

void InferSourceEntrySimulationFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    if (query.hasLineSensitiveLoopFallback) {
        return;
    }
    auto summaries = BuildSourceFunctionSummaries(lines);
    if (summaries.empty()) {
        return;
    }
    bool queryInsideKnownFunction = false;
    for (const auto& [name, summary] : summaries) {
        (void)name;
        if (summary.startLine != 0 && summary.endLine != 0 &&
            summary.startLine < query.line && query.line < summary.endLine) {
            queryInsideKnownFunction = true;
            break;
        }
    }
    if (!queryInsideKnownFunction) {
        return;
    }
    auto mainIt = summaries.find("main");
    if (mainIt == summaries.end() || !mainIt->second.params.empty() || mainIt->second.endLine == 0) {
        return;
    }
    if (SourceSummaryPrefixUnsupportedForLocalSimulation(mainIt->second, mainIt->second.endLine)) {
        return;
    }
    auto scopes = BuildSourceGlobalScopes(lines);
    auto observed = SimulateSourceFunctionSummaryAtLine(
        mainIt->second, scopes, summaries, query.line, query.variableName, 0);
    if (!observed.has_value()) {
        return;
    }
    SetSourceIntSetFallback(query, std::move(observed.value()));
    query.preferSourceFallback = query.hasSourceFallback;
    query.sourceFallbackMayBeLoopNarrow = query.hasSourceFallback;
}

void InferSourceFunctionContextQueryFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    auto summaries = BuildSourceFunctionSummaries(lines);
    if (summaries.empty()) {
        return;
    }

    const SourceFunctionSummary* targetSummary = nullptr;
    for (const auto& [name, summary] : summaries) {
        if (summary.startLine != 0 && summary.endLine != 0 &&
            summary.startLine < query.line && query.line < summary.endLine) {
            targetSummary = &summary;
            break;
        }
    }
    if (targetSummary == nullptr) {
        return;
    }

    std::vector<int64_t> values;
    std::vector<SourceScope> scopes(1);
    for (unsigned lineNo = 1; lineNo <= lines.size(); ++lineNo) {
        auto raw = lines[lineNo - 1];
        auto trimmed = Trim(StripLineComment(raw));
        if (trimmed.find("func ") != std::string::npos) {
            if (auto end = FindSourceBraceBlockEnd(lines, lineNo); end.has_value() && end.value() >= lineNo) {
                lineNo = end.value();
                continue;
            }
        }

        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(raw, name, typeHint, expr)) {
            CollectSourceFunctionContextValuesFromExpr(expr, scopes, summaries, query, values);
            if (values.size() > MAX_CONTEST_EXACT_VALUES) {
                return;
            }
            if (auto exact = EvalSourceExactForSimulation(expr, scopes, summaries, typeHint, 0); exact.has_value()) {
                scopes.back()[name] = exact.value();
            }
        } else if (ParseSourceAssignment(raw, name, expr)) {
            CollectSourceFunctionContextValuesFromExpr(expr, scopes, summaries, query, values);
            if (values.size() > MAX_CONTEST_EXACT_VALUES) {
                return;
            }
            auto old = LookupSourceValue(scopes, name);
            auto hint = old == nullptr ? ContestQueryTypeHint::UNKNOWN : old->typeHint;
            if (auto exact = EvalSourceExactForSimulation(expr, scopes, summaries, hint, 0); exact.has_value()) {
                SetSourceValueInNearestScope(scopes, name, exact.value());
            } else {
                EraseSourceValueFromScopes(scopes, name);
            }
        } else if (auto ret = ExtractSourceReturnExpr(trimmed); ret.has_value()) {
            CollectSourceFunctionContextValuesFromExpr(ret.value(), scopes, summaries, query, values);
            if (values.size() > MAX_CONTEST_EXACT_VALUES) {
                return;
            }
        }

        for (auto c : raw) {
            if (c == '{') {
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                scopes.pop_back();
            }
        }
    }
    auto normalized = NormalizeSourceIntSet(std::move(values));
    if (normalized.has_value()) {
        SetSourceIntSetFallback(query, std::move(normalized.value()));
        query.preferSourceFallback = query.hasSourceFallback;
    }
}

void InferSourceFunctionCallFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    auto line = lines[query.line - 1];
    bool matched = ParseSourceDeclaration(line, name, typeHint, expr) && name == query.variableName;
    if (!matched) {
        matched = ParseSourceAssignment(line, name, expr) && name == query.variableName;
    }
    if (matched) {
        expr = NormalizeSourceTrailingClosureCall(expr);
    }
    if (!matched || expr.find('(') == std::string::npos) {
        return;
    }
    auto summaries = BuildSourceFunctionSummaries(lines);
    if (summaries.empty()) {
        return;
    }
    std::vector<int64_t> values;
    auto baseScopes = BuildSourceScopesBeforeLine(lines, query.line);
    auto loop = FindInnermostSourceLoop(lines, query.line);
    auto forRange = loop.has_value() ? ParseSourceForInRangeHeader(lines[loop->start - 1]) : std::optional<SourceForRangeInfo>{};
    auto iterations = loop.has_value() ? GetSourceForRangeIterationValues(lines, loop.value()) : std::optional<std::vector<int64_t>>{};
    bool usedFunctionSimulation = false;
    if (forRange.has_value() && iterations.has_value() && SourceExprContainsIdentifier(expr, forRange->variable)) {
        for (auto iterValue : iterations.value()) {
            auto scopes = baseScopes;
            SourceExactValue exact;
            exact.typeHint = ContestQueryTypeHint::INT64;
            exact.intValue = iterValue;
            scopes.back()[forRange->variable] = exact;
            auto current = EvalSourceIntExprSetWithFunctions(expr, scopes, summaries, 0, &usedFunctionSimulation);
            if (current.has_value()) {
                values.insert(values.end(), current->begin(), current->end());
            }
        }
    } else if (auto current = EvalSourceIntExprSetWithFunctions(
        expr, baseScopes, summaries, 0, &usedFunctionSimulation); current.has_value()) {
        values.insert(values.end(), current->begin(), current->end());
    }
    if (!values.empty()) {
        SetSourceIntSetFallback(query, std::move(values));
        if (usedFunctionSimulation) {
            query.preferSourceFallback = query.hasSourceFallback;
        }
    }
}

void CollectSourceInlineTryValues(
    const std::string& expr, const std::vector<SourceScope>& scopes, std::vector<int64_t>& values)
{
    auto trimmed = Trim(expr);
    auto tryPos = trimmed.find("try");
    if (tryPos == std::string::npos) {
        return;
    }
    size_t pos = tryPos;
    while (pos < trimmed.size()) {
        auto open = trimmed.find('{', pos);
        if (open == std::string::npos) {
            break;
        }
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < trimmed.size(); ++i) {
            if (trimmed[i] == '{') {
                ++depth;
            } else if (trimmed[i] == '}') {
                --depth;
                if (depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        if (close == std::string::npos || close <= open) {
            break;
        }
        auto body = Trim(trimmed.substr(open + 1, close - open - 1));
        if (body.rfind("return ", 0) == 0) {
            body = Trim(body.substr(7));
        }
        if (auto arrow = body.find("=>"); arrow != std::string::npos) {
            body = TrimSourceMatchArmExpr(body.substr(arrow + 2));
        }
        if (auto semicolon = body.rfind(';'); semicolon != std::string::npos) {
            body = Trim(body.substr(semicolon + 1));
        }
        if (auto value = EvalSourceIntExpr(body, scopes); value.has_value()) {
            values.emplace_back(value.value());
        } else if (auto spawnValue = EvalSourceInlineSpawnExpr(body, scopes); spawnValue.has_value()) {
            values.emplace_back(spawnValue.value());
        }
        pos = close + 1;
        auto rest = Trim(trimmed.substr(pos));
        if (rest.rfind("catch", 0) != 0 && rest.rfind("finally", 0) != 0) {
            break;
        }
    }
}

void AddSourceTryLineValue(const std::vector<std::string>& lines, unsigned lineNo,
    const std::string& expr, const std::vector<SourceScope>& scopes, std::vector<int64_t>& values)
{
    if (auto value = EvalSourceIntExpr(expr, scopes); value.has_value()) {
        values.emplace_back(value.value());
        return;
    }
    if (expr.find("spawn") == std::string::npos) {
        return;
    }
    if (auto inlineSpawn = EvalSourceInlineSpawnExpr(expr, scopes); inlineSpawn.has_value()) {
        values.emplace_back(inlineSpawn.value());
        return;
    }
    if (auto blockSpawn = EvalSourceSpawnExprBlock(lines, lineNo, scopes); blockSpawn.has_value()) {
        values.emplace_back(blockSpawn.value());
    }
}

void InferSourceTryFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    std::string name;
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string expr;
    auto line = lines[query.line - 1];
    bool matched = ParseSourceDeclaration(line, name, typeHint, expr) && name == query.variableName;
    if (!matched) {
        matched = ParseSourceAssignment(line, name, expr) && name == query.variableName;
    }
    if (!matched || expr.find("try") == std::string::npos) {
        return;
    }
    auto scopes = BuildSourceScopesBeforeLine(lines, query.line);
    std::vector<int64_t> values;
    auto limit = std::min<unsigned>(static_cast<unsigned>(lines.size()), query.line + 64);
    int depth = 0;
    bool sawTryOpen = false;
    for (unsigned lineNo = query.line; lineNo <= limit; ++lineNo) {
        auto raw = StripLineComment(lines[lineNo - 1]);
        std::string assigned;
        std::string assignedExpr;
        if (ParseSourceAssignment(raw, assigned, assignedExpr) && assigned == query.variableName) {
            CollectSourceInlineTryValues(assignedExpr, scopes, values);
            AddSourceTryLineValue(lines, lineNo, assignedExpr, scopes, values);
        }
        auto trimmed = Trim(raw);
        if (trimmed.rfind("return ", 0) == 0) {
            trimmed = Trim(trimmed.substr(7));
        }
        trimmed = TrimSourceMatchArmExpr(trimmed);
        AddSourceTryLineValue(lines, lineNo, trimmed, scopes, values);
        for (auto c : raw) {
            if (c == '{') {
                ++depth;
                sawTryOpen = true;
            } else if (c == '}' && depth > 0) {
                --depth;
            }
        }
        if (sawTryOpen && depth == 0 && lineNo > query.line) {
            unsigned next = lineNo + 1;
            while (next <= lines.size() && Trim(StripLineComment(lines[next - 1])).empty()) {
                ++next;
            }
            if (next > lines.size()) {
                break;
            }
            auto nextLine = Trim(StripLineComment(lines[next - 1]));
            if (nextLine.find("catch") == std::string::npos && nextLine.find("finally") == std::string::npos) {
                break;
            }
        }
    }
    if (!values.empty()) {
        SetSourceIntSetFallback(query, std::move(values));
    }
}


void MergePriorSourceFallback(ContestQuery& query, const ContestQuery& candidate)
{
    if (candidate.hasSourceFallback) {
        query.sourceFallback = candidate.sourceFallback;
        query.hasSourceFallback = true;
        query.preferSourceFallback = query.preferSourceFallback || candidate.preferSourceFallback;
        query.sourceFallbackMayBeLoopNarrow =
            query.sourceFallbackMayBeLoopNarrow || candidate.sourceFallbackMayBeLoopNarrow;
    }
    if (candidate.hasAccumulatorFallback) {
        query.accumulatorFallback = candidate.accumulatorFallback;
        query.hasAccumulatorFallback = true;
    }
}

void InferPriorSourceAssignmentFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size() || query.typeHint == ContestQueryTypeHint::BOOL) {
        return;
    }
    unsigned begin = query.line > 512 ? query.line - 512 : 1;
    for (unsigned lineNo = begin; lineNo < query.line && lineNo <= lines.size(); ++lineNo) {
        if (!SourceLineAssignsVariableName(lines[lineNo - 1], query.variableName)) {
            continue;
        }
        ContestQuery candidate = query;
        candidate.line = lineNo;
        candidate.sourceLine = lines[lineNo - 1];
        candidate.sourceFallback.clear();
        candidate.accumulatorFallback.clear();
        candidate.hasSourceFallback = false;
        candidate.hasAccumulatorFallback = false;
        candidate.preferSourceFallback = false;
        candidate.sourceFallbackMayBeLoopNarrow = false;
        candidate.resolved = false;
        InferSourceAssignedValuesFallback(lines, candidate);
        InferSourceMatchFallback(lines, candidate);
        InferSourceFunctionCallFallback(lines, candidate);
        InferSourceTryFallback(lines, candidate);
        MergePriorSourceFallback(query, candidate);
    }
}

void InferContestQuerySourceFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size()) {
        return;
    }
    auto setSourceFallback = [&query](const SourceExactValue& value) {
        query.sourceFallback = value.typeHint == ContestQueryTypeHint::BOOL
            ? (value.boolValue ? "true" : "false")
            : std::to_string(value.intValue);
        query.hasSourceFallback = true;
    };
    const bool queryLineInsideLoop = IsSourceLineInsideLoop(lines, query.line);
    const bool hasPriorLoopAssignment = !queryLineInsideLoop &&
        SourceHasPriorLoopAssignment(lines, query.line, query.variableName);
    std::vector<SourceScope> scopes(1);
    for (unsigned lineNo = 1; lineNo <= query.line; ++lineNo) {
        auto line = lines[lineNo - 1];
        if (lineNo == query.line && !queryLineInsideLoop && !hasPriorLoopAssignment &&
            !SourceLineDeclaresVariable(line, query.variableName)) {
            if (auto value = LookupSourceValue(scopes, query.variableName); value != nullptr) {
                setSourceFallback(*value);
            }
        }
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(line, name, typeHint, expr)) {
            if (lineNo == query.line && name == query.variableName && expr.find("try") != std::string::npos) {
                std::vector<int64_t> values;
                CollectSourceInlineTryValues(expr, scopes, values);
                if (!values.empty()) {
                    SetSourceIntSetFallback(query, std::move(values));
                }
            }
            SourceExactValue value;
            auto overflowStrategy = GetSourceOverflowStrategyAtLine(lines, lineNo);
            bool hasExact = TryEvalSourceExactValue(expr, scopes, typeHint, value, overflowStrategy);
            if (!hasExact && expr.find("spawn") != std::string::npos) {
                if (auto spawnValue = EvalSourceSpawnExprBlock(lines, lineNo, scopes); spawnValue.has_value()) {
                    value.typeHint = ContestQueryTypeHint::INT64;
                    value.intValue = spawnValue.value();
                    hasExact = true;
                }
            }
            if (hasExact) {
                scopes.back()[name] = value;
                if (lineNo == query.line && name == query.variableName &&
                    !ShouldSuppressMutableDeclarationFallback(lines, query, lineNo, name)) {
                    setSourceFallback(value);
                }
            }
        }
        for (auto c : line) {
            if (c == '{') {
                scopes.emplace_back();
            } else if (c == '}' && scopes.size() > 1) {
                scopes.pop_back();
            }
        }
    }
}

// 读取查询所在源码行，在 CHIR Debug 缺失时保留 Bool/整数 fallback 所需类型。
void InferContestQueryTypeHintFromSource(ContestQuery& query,
    std::unordered_map<std::string, std::vector<std::string>>& sourceCache, const std::filesystem::path& contestRoot)
{
    if (!query.valid || query.fileName.empty() || query.line == 0) {
        return;
    }
    std::filesystem::path sourcePath(query.sourceFileName.empty() ? query.fileName : query.sourceFileName);
    if (sourcePath.is_relative()) {
        sourcePath = contestRoot / sourcePath;
    }
    auto sourceKey = sourcePath.lexically_normal().string();
    auto it = sourceCache.find(sourceKey);
    if (it == sourceCache.end()) {
        std::vector<std::string> lines;
        std::ifstream source(sourceKey);
        std::string sourceLine;
        while (std::getline(source, sourceLine)) {
            lines.emplace_back(sourceLine);
        }
        it = sourceCache.emplace(sourceKey, std::move(lines)).first;
    }
    if (query.line > it->second.size()) {
        return;
    }
    query.sourceLine = it->second[query.line - 1];
    query.typeHint = InferContestQueryTypeHintFromLine(query.sourceLine, query.variableName);
    query.suppressNarrowLoopOutput = SourceHasUnsafePriorLoopAssignment(it->second, query.line, query.variableName);
    InferContestQuerySourceFallback(it->second, query);
    InferSourceForRangeVariableFallback(it->second, query);
    InferSourceAssignedValuesFallback(it->second, query);
    InferSourcePairLoopFallback(it->second, query);
    InferSourceMatchFallback(it->second, query);
    InferSourceGlobalConstantFallback(it->second, query);
    InferSourceFunctionCallFallback(it->second, query);
    InferSourceFunctionContextQueryFallback(it->second, query);
    InferSourceLocalSimulationFallback(it->second, query);
    InferSourceEntrySimulationFallback(it->second, query);
    InferSourcePriorLoopValueFallback(it->second, query);
    InferSourceTryFallback(it->second, query);
    InferPriorSourceAssignmentFallback(it->second, query);
    InferSourceAccumulatorFallback(it->second, query);
}

// 构造无效查询占位，保证输出行数与输入行数一致。
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
    std::unordered_map<std::string, std::vector<std::string>> sourceCache;
    std::string line;
    while (std::getline(input, line)) {
        auto query = ParseContestQueryLine(line, inputContext.rootPath);
        InferContestQueryTypeHintFromSource(query, sourceCache, inputContext.rootPath);
        queries.emplace_back(std::move(query));
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
std::string FormatFallback(ContestQueryTypeHint typeHint)
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

// 优先使用 CHIR Type*，缺失时使用源码类型提示生成 fallback。
std::string FormatFallback(const ContestQuery& query)
{
    if (query.type != nullptr) {
        return FormatFallback(query.type);
    }
    return FormatFallback(query.typeHint);
}

// 将抽象值域转换为竞赛要求的输出格式。
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
        return FormatFallback(type);
    }
    return FormatFallback(type);
}

// 读取普通 SSA 值或 ref 背后 var 对象的竞赛可见值域。
// Let a later precise contextual answer replace an earlier full-range answer.
bool IsContestFullBoundaryOutput(const std::string& output)
{
    return output.find("-9223372036854775808") != std::string::npos ||
        output.find("9223372036854775807") != std::string::npos;
}

bool IsContestIntervalOutput(const std::string& output)
{
    return !output.empty() && output.front() == '[';
}

bool IsContestSingletonOutput(const std::string& output)
{
    if (output.empty() || IsContestIntervalOutput(output)) {
        return false;
    }
    return output.find(',') == std::string::npos;
}

bool ShouldRecordContestResult(const ContestQuery& query, const std::string& result, Type* type)
{
    if (!query.resolved) {
        return true;
    }
    auto fallback = FormatFallback(type);
    return query.result == fallback && result != fallback;
}

bool HasUsableResolvedContestOutput(const ContestQuery& query)
{
    if (!query.resolved) {
        return false;
    }
    auto fallback = FormatFallback(query);
    auto typeFallback = query.type != nullptr ? FormatFallback(query.type) : fallback;
    return query.result != fallback && query.result != typeFallback && !IsContestFullBoundaryOutput(query.result);
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

void RememberContestAggregateFirstLine(
    ContestAggregateMap& aggregates, const DebugLocation& location, const std::string& fileKey,
    const std::string& variableName, Type* type)
{
    auto& aggregate = aggregates[MakeContestAggregateKey(fileKey, location.GetBeginPos().line, variableName)];
    aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
    if (aggregate.firstDebugLine == std::numeric_limits<unsigned>::max()) {
        aggregate.exactValues.reset();
    }
    aggregate.firstDebugLine = std::min(aggregate.firstDebugLine, location.GetBeginPos().line);
}

void RecordContestAggregateValue(ContestAggregateMap& aggregates, const ValueNameMap& valueNames,
    const DebugLocation& location, Value* value, const RangeDomain& state)
{
    auto names = valueNames.find(value);
    if (names == valueNames.end()) {
        return;
    }
    auto range = GetContestRangeForValue(state, value);
    if (range == nullptr || range->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return;
    }
    const auto& sintRange = StaticCast<const SIntRange&>(*range);
    if (!sintRange.GetExactValues().has_value()) {
        return;
    }
    auto type = GetQueryValueType(value);
    for (const auto& info : names->second) {
        auto& aggregate = aggregates[MakeContestAggregateKey(info.fileKey, info.line, info.name)];
        aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
        if (aggregate.firstDebugLine != std::numeric_limits<unsigned>::max() &&
            location.GetBeginPos().line == aggregate.firstDebugLine) {
            continue;
        }
        aggregate.exactValues = MergeContestExactValues(aggregate.exactValues, *sintRange.GetExactValues());
    }
}

void RecordContestSourceLineCandidate(
    ContestAggregateMap& aggregates, ContestQuery& query, Value* value, const RangeDomain& state)
{
    if (!query.valid || query.sourceLine.empty() || value == nullptr ||
        !SourceLineDeclaresVariable(query.sourceLine, query.variableName)) {
        return;
    }
    auto type = GetQueryValueType(value);
    if (type == nullptr || !type->IsInteger()) {
        return;
    }
    auto typeHint = GetQueryTypeHint(type);
    if (query.typeHint != ContestQueryTypeHint::UNKNOWN && query.typeHint != typeHint) {
        return;
    }
    auto range = GetContestRangeForValue(state, value);
    if (range == nullptr || range->GetRangeKind() != ValueRange::RangeKind::SINT) {
        return;
    }
    const auto& sintRange = StaticCast<const SIntRange&>(*range);
    if (!sintRange.GetExactValues().has_value()) {
        return;
    }
    auto& aggregate = aggregates[MakeContestAggregateKey(query.fileKey, query.line, query.variableName)];
    if (aggregate.firstDebugLine != std::numeric_limits<unsigned>::max()) {
        return;
    }
    aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
    aggregate.exactValues = MergeContestExactValues(aggregate.exactValues, *sintRange.GetExactValues());
}

void ApplyContestAggregates(std::vector<ContestQuery>& queries, const ContestAggregateMap& aggregates)
{
    for (auto& query : queries) {
        auto it = aggregates.find(MakeContestAggregateKey(query.fileKey, query.line, query.variableName));
        if (it == aggregates.end() || !it->second.exactValues.has_value() || it->second.exactValues->size() <= 1) {
            continue;
        }
        if (query.resolved && query.line != it->second.firstDebugLine) {
            continue;
        }
        auto type = query.type == nullptr ? it->second.type : query.type;
        if (type == nullptr || !type->IsInteger()) {
            continue;
        }
        query.type = type;
        query.typeHint = GetQueryTypeHint(type);
        query.result = FormatExactSIntValues(*it->second.exactValues, *type);
        query.resolved = true;
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
        debug.GetDebugLocation().GetBeginPos().line};
    auto sameInfo = [&info](const auto& old) {
        return old.name == info.name && old.fileKey == info.fileKey && old.line == info.line;
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

// 解析与具体 Debug 表达式位置和变量名匹配的查询。
void ResolveQueryAtDebug(std::vector<ContestQuery>& queries, ValueNameMap& valueNames,
    ContestAggregateMap& aggregates, const Debug& debug, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    RememberValueName(valueNames, debug, contestRoot);
    RememberQueryType(queries, debug, contestRoot);
    auto fileKey = GetLocationFileKey(debug.GetDebugLocation(), contestRoot);
    RememberContestAggregateFirstLine(
        aggregates, debug.GetDebugLocation(), fileKey, debug.GetSrcCodeIdentifier(), GetQueryValueType(debug.GetValue()));
    if (debug.GetValue()->GetType()->IsRef()) {
        return;
    }
    auto type = GetQueryValueType(debug.GetValue());
    auto range = GetContestRangeForValue(state, debug.GetValue());
    for (auto& query : queries) {
        if (!query.valid || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (!IsSameQueryLocation(query, debug.GetDebugLocation(), contestRoot)) {
            continue;
        }
        auto result = FormatContestRange(range, type);
        if (!ShouldRecordContestResult(query, result, type)) {
            continue;
        }
        query.type = type;
        query.typeHint = GetQueryTypeHint(type);
        query.result = std::move(result);
        query.resolved = true;
    }
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
        auto result = FormatContestRange(GetContestRangeForValue(state, value), type);
        if (!ShouldRecordContestResult(query, result, type)) {
            continue;
        }
        query.type = type;
        query.typeHint = GetQueryTypeHint(type);
        query.result = std::move(result);
        query.resolved = true;
    }
}

// 在非 Debug 表达式上尝试解析所有 operand 形式查询。
void MergeContestContextCandidate(
    ContestContextCandidateMap& candidates, size_t queryIndex, Type* type, const ValueRange* range)
{
    if (type == nullptr || range == nullptr) {
        return;
    }
    auto& candidate = candidates[queryIndex];
    candidate.type = candidate.type == nullptr ? type : candidate.type;
    if (candidate.range == nullptr) {
        candidate.range = range->Clone();
        return;
    }
    if (candidate.range->GetRangeKind() != range->GetRangeKind()) {
        return;
    }
    if (auto joined = candidate.range->Join(*range); joined.has_value()) {
        candidate.range = std::move(joined.value());
    }
}

void CollectContextCandidateAtDebug(const std::vector<ContestQuery>& queries, ContestContextCandidateMap& candidates,
    ValueNameMap& valueNames, const Debug& debug, const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    RememberValueName(valueNames, debug, contestRoot);
    if (debug.GetValue()->GetType()->IsRef()) {
        return;
    }
    auto type = GetQueryValueType(debug.GetValue());
    auto range = GetContestRangeForValue(state, debug.GetValue());
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!query.valid || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (!IsSameQueryLocation(query, debug.GetDebugLocation(), contestRoot)) {
            continue;
        }
        MergeContestContextCandidate(candidates, index, type, range);
    }
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
        MergeContestContextCandidate(candidates, index, GetQueryValueType(value), GetContestRangeForValue(state, value));
    }
}

void CollectContextCandidateAtExpressionOperands(const std::vector<ContestQuery>& queries,
    ContestContextCandidateMap& candidates, const ValueNameMap& valueNames, const Expression& expr,
    const RangeDomain& state, const std::filesystem::path& contestRoot)
{
    for (auto operand : expr.GetOperands()) {
        CollectContextCandidateAtValue(queries, candidates, valueNames, expr.GetDebugLocation(), operand, state, contestRoot);
    }
}

void ResolveQueryAtExpressionOperands(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    ContestAggregateMap& aggregates, const Expression& expr, const RangeDomain& state,
    const std::filesystem::path& contestRoot)
{
    for (auto operand : expr.GetOperands()) {
        RecordContestAggregateValue(aggregates, valueNames, expr.GetDebugLocation(), operand, state);
        ResolveQueryAtValue(queries, valueNames, expr.GetDebugLocation(), operand, state, contestRoot);
    }
    for (auto& query : queries) {
        if (!IsSameQueryLocation(query, expr.GetDebugLocation(), contestRoot)) {
            continue;
        }
        RecordContestSourceLineCandidate(aggregates, query, expr.GetResult(), state);
    }
}

// 带 visited 集合检查 CFG 可达性，避免环路递归。
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
    std::unordered_set<const Block*> visited;
    if (CanReachBlock(branch->GetTrueBlock(), parent, visited)) {
        return true;
    }
    visited.clear();
    return CanReachBlock(branch->GetFalseBlock(), parent, visited);
}

// 按查询顺序写入 output.txt，未解析项使用 fallback。
std::string GetContestQueryOutput(const ContestQuery& query)
{
    if (query.sourceFallbackMayBeLoopNarrow && query.hasAccumulatorFallback &&
        (!query.hasSourceFallback || IsContestSingletonOutput(query.sourceFallback))) {
        return query.accumulatorFallback;
    }
    if (query.preferSourceFallback && query.hasSourceFallback) {
        if (query.sourceFallbackMayBeLoopNarrow && IsContestSingletonOutput(query.sourceFallback) &&
            HasUsableResolvedContestOutput(query)) {
            return query.result;
        }
        return query.sourceFallback;
    }
    if (query.suppressNarrowLoopOutput) {
        return FormatFallback(query);
    }
    if (query.resolved) {
        auto fallback = FormatFallback(query);
        auto typeFallback = query.type != nullptr ? FormatFallback(query.type) : fallback;
        if (query.result != fallback && query.result != typeFallback) {
            return query.result;
        }
    }
    if (query.hasSourceFallback) {
        return query.sourceFallback;
    }
    if (query.resolved) {
        return query.result;
    }
    return FormatFallback(query);
}

bool IsContestFallbackOutput(const ContestQuery& query, const std::string& output)
{
    if (query.type != nullptr && output == FormatFallback(query.type)) {
        return true;
    }
    return output == FormatFallback(query);
}

bool ShouldUseAccumulatorFallback(const ContestQuery& query, const std::string& current)
{
    if (!query.hasAccumulatorFallback) {
        return false;
    }
    if (IsContestFallbackOutput(query, current)) {
        return true;
    }
    return query.sourceFallbackMayBeLoopNarrow && IsContestSingletonOutput(current);
}

bool ShouldApplyContestContextCandidate(const ContestQuery& query, const std::string& result, Type* type)
{
    if (result.empty() || result == FormatFallback(type) || IsContestSingletonOutput(result)) {
        return false;
    }
    if (!query.resolved) {
        return true;
    }
    if (query.result == FormatFallback(type) || IsContestFullBoundaryOutput(query.result)) {
        return true;
    }
    return false;
}

void ApplyContestContextCandidates(std::vector<ContestQuery>& queries, const ContestContextCandidateMap& candidates)
{
    for (const auto& [index, candidate] : candidates) {
        if (index >= queries.size() || candidate.range == nullptr) {
            continue;
        }
        auto& query = queries[index];
        auto result = FormatContestRange(candidate.range.get(), candidate.type);
        if (!ShouldApplyContestContextCandidate(query, result, candidate.type)) {
            continue;
        }
        query.type = candidate.type;
        query.typeHint = GetQueryTypeHint(candidate.type);
        query.result = std::move(result);
        query.resolved = true;
    }
}

std::vector<std::string> ReadExistingContestOutput(const std::filesystem::path& outputPath)
{
    std::vector<std::string> lines;
    std::ifstream input(outputPath.string());
    std::string line;
    while (std::getline(input, line)) {
        lines.emplace_back(line);
    }
    return lines;
}

void WriteContestOutput(std::vector<ContestQuery>& queries, const ContestInputContext& inputContext)
{
    auto outputPath = inputContext.rootPath / CONTEST_OUTPUT_FILE;
    auto existingLines = ReadExistingContestOutput(outputPath);
    std::ofstream output(outputPath.string(), std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    for (size_t index = 0; index < queries.size(); ++index) {
        auto& query = queries[index];
        auto current = GetContestQueryOutput(query);
        if (ShouldUseAccumulatorFallback(query, current)) {
            current = query.accumulatorFallback;
        }
        if (index < existingLines.size() && IsContestFallbackOutput(query, current) &&
            !IsContestFallbackOutput(query, existingLines[index])) {
            current = existingLines[index];
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
    const auto actionBeforeVisitExpr = [](const RangeDomain&, Expression*, size_t) {};
    ValueNameMap valueNames;
    ContestAggregateMap aggregates;
    auto actionAfterVisitExpr = [&queries, &valueNames, &aggregates, &contestRoot](
                                    const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
            ResolveQueryAtDebug(queries.value(), valueNames, aggregates, *StaticCast<Debug*>(expr), state, contestRoot);
            return;
        }
        ResolveQueryAtExpressionOperands(queries.value(), valueNames, aggregates, *expr, state, contestRoot);
    };
    const auto actionOnTerminator = [&queries, &valueNames, &aggregates, &contestRoot](
                                        const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
        if (terminator == nullptr) {
            return;
        }
        ResolveQueryAtExpressionOperands(queries.value(), valueNames, aggregates, *terminator, state, contestRoot);
    };
    auto resolveQueries = [&](Results<RangeDomain>& result) {
        result.VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
    };
    ContestContextCandidateMap contextCandidates;
    ValueNameMap contextValueNames;
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
    const auto contextActionOnTerminator = [&queries, &contextCandidates, &contextValueNames, &contestRoot](
                                               const RangeDomain& state, Terminator* terminator, std::optional<Block*>) {
        if (terminator == nullptr) {
            return;
        }
        CollectContextCandidateAtExpressionOperands(
            queries.value(), contextCandidates, contextValueNames, *terminator, state, contestRoot);
    };
    auto collectContextCandidates = [&]() {
        RangeAnalysis::VisitContextSensitiveResults(
            [&](const Function*, const std::string&, Results<RangeDomain>& result) {
                result.VisitWith(actionBeforeVisitExpr, contextActionAfterVisitExpr, contextActionOnTerminator);
            });
    };
    auto relevantFunctions = CollectContestRelevantFunctions(package, queries.value(), contestRoot);

    for (auto func : package->GetGlobalFuncsWithBody()) {
        if (!RangeAnalysis::Filter(*func) || relevantFunctions.find(func) == relevantFunctions.end()) {
            continue;
        }
        auto result = rangeAnalysisWrapper.RunOnFunc(func, /* isDebug = */ false, diag);
        if (result != nullptr) {
            resolveQueries(*result);
            collectContextCandidates();
        } else if (auto cachedResult = rangeAnalysisWrapper.CheckFuncResult(func); cachedResult != nullptr) {
            resolveQueries(*cachedResult);
            collectContextCandidates();
        }
        RangeAnalysis::ClearContextSensitiveResults();
    }

    ApplyContestContextCandidates(queries.value(), contextCandidates);
    ApplyContestAggregates(queries.value(), aggregates);
    WriteContestOutput(queries.value(), inputContext.value());
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
