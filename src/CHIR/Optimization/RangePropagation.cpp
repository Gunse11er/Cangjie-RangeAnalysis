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

namespace Cangjie::CHIR {

namespace {
const std::string CONTEST_INPUT_FILE = "input.txt";
const std::string CONTEST_OUTPUT_FILE = "output.txt";

std::optional<std::filesystem::path> FindContestInputFile()
{
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (ec) {
        return std::nullopt;
    }
    while (true) {
        auto candidate = current / CONTEST_INPUT_FILE;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
        ec.clear();
        if (current == current.root_path()) {
            break;
        }
        auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
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

struct ContestQuery {
    std::string fileName;
    std::string sourceFileName;
    unsigned line{0};
    std::string variableName;
    std::string result;
    Type* type{nullptr};
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    std::string sourceFallback;
    bool valid{true};
    bool resolved{false};
    bool hasSourceFallback{false};
};

using ValueNameMap = std::unordered_map<Value*, std::vector<std::string>>;

struct ContestAggregate {
    Type* type{nullptr};
    unsigned firstDebugLine{std::numeric_limits<unsigned>::max()};
    std::optional<std::vector<SInt>> exactValues;
};

using ContestAggregateMap = std::unordered_map<std::string, ContestAggregate>;
constexpr size_t MAX_CONTEST_EXACT_VALUES = 16;

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

std::string MakeContestAggregateKey(const std::string& fileName, const std::string& variableName)
{
    return fileName + "\0" + variableName;
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

struct SourceExactValue {
    ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
    int64_t intValue{0};
    bool boolValue{false};
};

using SourceScope = std::unordered_map<std::string, SourceExactValue>;

// 从内向外查找源码常量环境中已经证明的简单值。
const SourceExactValue* LookupSourceValue(const std::vector<SourceScope>& scopes, const std::string& name)
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto value = it->find(name);
        if (value != it->end()) {
            return &value->second;
        }
    }
    return nullptr;
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
    const std::string letPrefix = "let ";
    const std::string varPrefix = "var ";
    size_t pos = std::string::npos;
    if (trimmed.rfind(letPrefix, 0) == 0) {
        pos = letPrefix.size();
    } else if (trimmed.rfind(varPrefix, 0) == 0) {
        pos = varPrefix.size();
    } else {
        return false;
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
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos >= trimmed.size() || trimmed[pos] != ':') {
        return false;
    }
    ++pos;
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    auto typeBegin = pos;
    while (pos < trimmed.size() && IsIdentifierChar(trimmed[pos])) {
        ++pos;
    }
    typeHint = ParseContestQueryTypeHint(trimmed.substr(typeBegin, pos - typeBegin));
    if (typeHint == ContestQueryTypeHint::UNKNOWN) {
        return false;
    }
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos >= trimmed.size() || trimmed[pos] != '=') {
        return false;
    }
    expr = Trim(trimmed.substr(pos + 1));
    return !expr.empty();
}

// 解析源码中的十进制 Int64 字面量。
std::optional<int64_t> ParseSourceIntLiteral(const std::string& text)
{
    auto trimmed = Trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    size_t pos = trimmed[0] == '-' ? 1 : 0;
    if (pos >= trimmed.size()) {
        return std::nullopt;
    }
    for (size_t i = pos; i < trimmed.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
            return std::nullopt;
        }
    }
    try {
        return std::stoll(trimmed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int64_t> EvalSourceIntExpr(const std::string& expr, const std::vector<SourceScope>& scopes);

// 查找简单二元运算符位置，跳过表达式开头的一元负号。
size_t FindSourceBinaryOperator(const std::string& expr, char op)
{
    for (size_t i = 1; i < expr.size(); ++i) {
        if (expr[i] == op) {
            return i;
        }
    }
    return std::string::npos;
}

// 保守求值源码中的简单 Int64 常量表达式。
std::optional<int64_t> EvalSourceIntExpr(const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = Trim(expr);
    if (auto pos = FindSourceBinaryOperator(trimmed, '+'); pos != std::string::npos) {
        auto lhs = EvalSourceIntExpr(trimmed.substr(0, pos), scopes);
        auto rhs = EvalSourceIntExpr(trimmed.substr(pos + 1), scopes);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        int64_t result = 0;
        if (__builtin_add_overflow(lhs.value(), rhs.value(), &result)) {
            return std::nullopt;
        }
        return result;
    }
    if (auto pos = FindSourceBinaryOperator(trimmed, '-'); pos != std::string::npos) {
        auto lhs = EvalSourceIntExpr(trimmed.substr(0, pos), scopes);
        auto rhs = EvalSourceIntExpr(trimmed.substr(pos + 1), scopes);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        int64_t result = 0;
        if (__builtin_sub_overflow(lhs.value(), rhs.value(), &result)) {
            return std::nullopt;
        }
        return result;
    }
    if (auto literal = ParseSourceIntLiteral(trimmed); literal.has_value()) {
        return literal;
    }
    if (auto value = LookupSourceValue(scopes, trimmed);
        value != nullptr && value->typeHint == ContestQueryTypeHint::INT64) {
        return value->intValue;
    }
    return std::nullopt;
}

// 保守求值源码中的简单 Bool 常量表达式。
std::optional<bool> EvalSourceBoolExpr(const std::string& expr, const std::vector<SourceScope>& scopes)
{
    auto trimmed = Trim(expr);
    if (trimmed == "true") {
        return true;
    }
    if (trimmed == "false") {
        return false;
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

// 当 CHIR 中查询点被优化掉时，为源码中的简单常量 let 提供精确 fallback。
void InferContestQuerySourceFallback(const std::vector<std::string>& lines, ContestQuery& query)
{
    if (!query.valid || query.line == 0 || query.line > lines.size()) {
        return;
    }
    std::vector<SourceScope> scopes(1);
    for (unsigned lineNo = 1; lineNo <= query.line; ++lineNo) {
        auto line = lines[lineNo - 1];
        std::string name;
        ContestQueryTypeHint typeHint{ContestQueryTypeHint::UNKNOWN};
        std::string expr;
        if (ParseSourceDeclaration(line, name, typeHint, expr)) {
            SourceExactValue value;
            value.typeHint = typeHint;
            bool hasValue = false;
            if (typeHint == ContestQueryTypeHint::INT64) {
                if (auto intValue = EvalSourceIntExpr(expr, scopes); intValue.has_value()) {
                    value.intValue = intValue.value();
                    hasValue = true;
                }
            } else if (typeHint == ContestQueryTypeHint::BOOL) {
                if (auto boolValue = EvalSourceBoolExpr(expr, scopes); boolValue.has_value()) {
                    value.boolValue = boolValue.value();
                    hasValue = true;
                }
            }
            if (hasValue) {
                scopes.back()[name] = value;
                if (lineNo == query.line && name == query.variableName) {
                    query.sourceFallback = typeHint == ContestQueryTypeHint::BOOL
                        ? (value.boolValue ? "true" : "false")
                        : std::to_string(value.intValue);
                    query.hasSourceFallback = true;
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
    query.typeHint = InferContestQueryTypeHintFromLine(it->second[query.line - 1], query.variableName);
    InferContestQuerySourceFallback(it->second, query);
}

// 构造无效查询占位，保证输出行数与输入行数一致。
ContestQuery MakeInvalidContestQuery()
{
    ContestQuery query;
    query.valid = false;
    return query;
}

// 解析一行 [file, line, variable] 格式的竞赛查询。
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
    query.sourceFileName = parts[0];
    query.fileName = BaseName(query.sourceFileName);
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
std::optional<std::vector<ContestQuery>> LoadContestQueries()
{
    auto inputPath = FindContestInputFile();
    if (!inputPath.has_value()) {
        return std::nullopt;
    }
    std::ifstream input(inputPath.value().string());
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::vector<ContestQuery> queries;
    std::unordered_map<std::string, std::vector<std::string>> sourceCache;
    std::string line;
    while (std::getline(input, line)) {
        auto query = ParseContestQueryLine(line);
        InferContestQueryTypeHintFromSource(query, sourceCache, inputPath.value().parent_path());
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
bool ShouldRecordContestResult(const ContestQuery& query, const std::string& result, Type* type)
{
    if (!query.resolved) {
        return true;
    }
    auto fallback = FormatFallback(type);
    return query.result == fallback && result != fallback;
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
    ContestAggregateMap& aggregates, const DebugLocation& location, const std::string& variableName, Type* type)
{
    auto& aggregate = aggregates[MakeContestAggregateKey(BaseName(location.GetFileName()), variableName)];
    aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
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
    auto fileName = BaseName(location.GetFileName());
    for (const auto& name : names->second) {
        auto& aggregate = aggregates[MakeContestAggregateKey(fileName, name)];
        aggregate.type = aggregate.type == nullptr ? type : aggregate.type;
        if (aggregate.firstDebugLine != std::numeric_limits<unsigned>::max() &&
            location.GetBeginPos().line == aggregate.firstDebugLine) {
            continue;
        }
        aggregate.exactValues = MergeContestExactValues(aggregate.exactValues, *sintRange.GetExactValues());
    }
}

void ApplyContestAggregates(std::vector<ContestQuery>& queries, const ContestAggregateMap& aggregates)
{
    for (auto& query : queries) {
        auto it = aggregates.find(MakeContestAggregateKey(query.fileName, query.variableName));
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
bool IsSameQueryLocation(const ContestQuery& query, const DebugLocation& location)
{
    return query.fileName == BaseName(location.GetFileName()) && query.line == location.GetBeginPos().line;
}

// 判断某个值是否已关联指定源码变量名。
bool HasValueName(const ValueNameMap& valueNames, Value* value, const std::string& name)
{
    auto it = valueNames.find(value);
    if (it == valueNames.end()) {
        return false;
    }
    return std::find(it->second.begin(), it->second.end(), name) != it->second.end();
}

// 记录 Debug(value, name) 映射，供后续 operand 查询解析使用。
void RememberValueName(ValueNameMap& valueNames, const Debug& debug)
{
    auto value = debug.GetValue();
    auto& names = valueNames[value];
    if (std::find(names.begin(), names.end(), debug.GetSrcCodeIdentifier()) == names.end()) {
        names.emplace_back(debug.GetSrcCodeIdentifier());
    }
}

// 记录查询类型，即使精确程序点解析失败也能正确 fallback。
void RememberQueryType(std::vector<ContestQuery>& queries, const Debug& debug)
{
    auto type = GetQueryValueType(debug.GetValue());
    auto typeHint = GetQueryTypeHint(type);
    for (auto& query : queries) {
        if (!query.valid || query.type || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (query.typeHint != ContestQueryTypeHint::UNKNOWN && query.typeHint != typeHint) {
            continue;
        }
        if (query.fileName == BaseName(debug.GetDebugLocation().GetFileName()) || query.fileName.empty()) {
            query.type = type;
            query.typeHint = typeHint;
        }
    }
}

// 解析与具体 Debug 表达式位置和变量名匹配的查询。
void ResolveQueryAtDebug(std::vector<ContestQuery>& queries, ValueNameMap& valueNames,
    ContestAggregateMap& aggregates, const Debug& debug, const RangeDomain& state)
{
    RememberValueName(valueNames, debug);
    RememberQueryType(queries, debug);
    RememberContestAggregateFirstLine(
        aggregates, debug.GetDebugLocation(), debug.GetSrcCodeIdentifier(), GetQueryValueType(debug.GetValue()));
    if (debug.GetValue()->GetType()->IsRef()) {
        return;
    }
    auto type = GetQueryValueType(debug.GetValue());
    auto range = GetContestRangeForValue(state, debug.GetValue());
    for (auto& query : queries) {
        if (!query.valid || query.variableName != debug.GetSrcCodeIdentifier()) {
            continue;
        }
        if (!IsSameQueryLocation(query, debug.GetDebugLocation())) {
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
    Value* value, const RangeDomain& state)
{
    for (auto& query : queries) {
        if (!query.valid || !HasValueName(valueNames, value, query.variableName)) {
            continue;
        }
        if (!IsSameQueryLocation(query, location)) {
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
void ResolveQueryAtExpressionOperands(std::vector<ContestQuery>& queries, const ValueNameMap& valueNames,
    ContestAggregateMap& aggregates, const Expression& expr, const RangeDomain& state)
{
    for (auto operand : expr.GetOperands()) {
        RecordContestAggregateValue(aggregates, valueNames, expr.GetDebugLocation(), operand, state);
        ResolveQueryAtValue(queries, valueNames, expr.GetDebugLocation(), operand, state);
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
void WriteContestOutput(std::vector<ContestQuery>& queries)
{
    auto inputPath = FindContestInputFile();
    auto outputPath = inputPath.has_value() ? inputPath.value().parent_path() / CONTEST_OUTPUT_FILE
                                            : std::filesystem::path(CONTEST_OUTPUT_FILE);
    std::ofstream output(outputPath.string(), std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    for (auto& query : queries) {
        if (query.resolved) {
            output << query.result << '\n';
        } else if (query.hasSourceFallback) {
            output << query.sourceFallback << '\n';
        } else {
            output << FormatFallback(query) << '\n';
        }
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
void RangePropagation::EmitContestOutput(const Ptr<const Package>& package, RangeAnalysisWrapper& rangeAnalysisWrapper)
{
    auto queries = LoadContestQueries();
    if (!queries.has_value()) {
        return;
    }
    const auto actionBeforeVisitExpr = [](const RangeDomain&, Expression*, size_t) {};
    ValueNameMap valueNames;
    ContestAggregateMap aggregates;
    auto actionAfterVisitExpr = [&queries, &valueNames, &aggregates](const RangeDomain& state, Expression* expr, size_t) {
        if (expr->GetExprKind() == ExprKind::DEBUGEXPR) {
            ResolveQueryAtDebug(queries.value(), valueNames, aggregates, *StaticCast<Debug*>(expr), state);
            return;
        }
        ResolveQueryAtExpressionOperands(queries.value(), valueNames, aggregates, *expr, state);
    };
    const auto actionOnTerminator = [](const RangeDomain&, Terminator*, std::optional<Block*>) {};
    auto resolveQueries = [&](Results<RangeDomain>& result) {
        result.VisitWith(actionBeforeVisitExpr, actionAfterVisitExpr, actionOnTerminator);
    };

    for (auto func : package->GetGlobalFuncsWithBody()) {
        auto result = rangeAnalysisWrapper.CheckFuncResult(func);
        if (!result) {
            continue;
        }
        resolveQueries(*result);
    }

    RangeAnalysis::VisitContextSensitiveResults(
        [&](const Function*, const std::string&, Results<RangeDomain>& result) { resolveQueries(result); });

    ApplyContestAggregates(queries.value(), aggregates);
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
