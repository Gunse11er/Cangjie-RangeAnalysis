// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This document aims to parse PackageConfig.toml (interop CJ package configuration information),
 * which primarily involves the symbols that the target language can expose in interoperability scenarios,
 * as well as the specific type sets for generic instantiation.
 */

#include "cangjie/Basic/InteropCJPackageConfigReader.h"
#include "cangjie/Utils/CheckUtils.h"
#include <iostream>
#include <stdexcept>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include <toml.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Cangjie {
using namespace toml;

namespace {
const std::string DEFAULT_SECTION = "default";
const std::string API_STRATEGY = "APIStrategy";
const std::string GENERIC_TYPE_STRATEGY = "GenericTypeStrategy";
const std::string PACKAGE_SECTION = "package";
const std::string PACKAGE_NAME = "name";
const std::string INCLUDED_APIS = "included_apis";
const std::string EXCLUDED_APIS = "excluded_apis";
const std::string GENERIC_OBJECT_CONFIG = "generic_object_configuration";
const std::string TUPLE_CONFIG = "tuple_configuration";
const std::string LAMBDA_PATTERNS = "lambda_patterns";
const std::string SIGNATURE = "signature";
const std::string CLASS_MAPPINGS = "class_mappings";
const std::string TYPE_ARGUMENTS = "type_arguments";
const std::string SYMBOLS = "symbols";

const std::string STRATEGY_FULL = "Full";
const std::string STRATEGY_NONE = "None";
const std::string GENERIC_STRATEGY_PARTIAL = "Partial";
const std::string GENERIC_STRATEGY_NONE = "None";

InteropCJStrategy StringToStrategy(const std::string& str)
{
    if (str == STRATEGY_FULL)
        return InteropCJStrategy::FULL;
    if (str == STRATEGY_NONE)
        return InteropCJStrategy::NONE;
    return InteropCJStrategy::UNKNOWN;
}

InteropCJGenericStrategyType StringToGenericStrategy(const std::string& str)
{
    if (str == GENERIC_STRATEGY_NONE)
        return InteropCJGenericStrategyType::NONE;
    if (str == GENERIC_STRATEGY_PARTIAL)
        return InteropCJGenericStrategyType::PARTIAL;
    return InteropCJGenericStrategyType::UNKNOWN;
}

InteropCJStrategy ParseAPIStrategy(toml::Table& packageTable)
{
    if (packageTable.find(API_STRATEGY) == packageTable.end() || !packageTable[API_STRATEGY].is<std::string>()) {
        return InteropCJStrategy::NONE;
    }

    auto strategy = packageTable[API_STRATEGY].as<std::string>();
    return StringToStrategy(strategy);
}

InteropCJGenericStrategyType ParseGenericTypeStrategy(toml::Table& packageTable)
{
    if (packageTable.find(GENERIC_TYPE_STRATEGY) == packageTable.end() ||
        !packageTable[GENERIC_TYPE_STRATEGY].is<std::string>()) {
        return InteropCJGenericStrategyType::NONE;
    }

    auto strategy = packageTable[GENERIC_TYPE_STRATEGY].as<std::string>();
    return StringToGenericStrategy(strategy);
}

void ParseIncludedAPIs(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    if (packageTable.find(INCLUDED_APIS) == packageTable.end() || !packageTable[INCLUDED_APIS].is<toml::Array>()) {
        return;
    }

    auto includedApis = packageTable[INCLUDED_APIS].as<toml::Array>();

    for (const auto& item : includedApis) {
        if (!item.is<std::string>()) {
            continue;
        }

        auto api = item.as<std::string>();
        pkgConfig.interopCJIncludedApis.push_back(api);
    }
}

void ParseExcludedAPIs(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    if (packageTable.find(EXCLUDED_APIS) == packageTable.end() || !packageTable[EXCLUDED_APIS].is<toml::Array>()) {
        return;
    }

    auto excludedApis = packageTable[EXCLUDED_APIS].as<toml::Array>();

    for (const auto& item : excludedApis) {
        if (!item.is<std::string>()) {
            continue;
        }

        auto api = item.as<std::string>();
        pkgConfig.interopCJExcludedApis.push_back(api);
    }
}

void ProcessGenericTypeWithSymbols(toml::Table& genTable, const std::string& fullName, size_t angleBracketPos,
    const std::unordered_map<std::string, std::vector<std::string>>& typeArgumentsMap, PackageConfig& pkgConfig)
{
    std::string outerType = fullName.substr(0, angleBracketPos);
    std::string innerType = fullName.substr(angleBracketPos + 1, fullName.size() - angleBracketPos - 2);

    auto it = typeArgumentsMap.find(outerType);
    if (it == typeArgumentsMap.end()) {
        return;
    }

    const auto& allowedTypes = it->second;
    if (std::find(allowedTypes.begin(), allowedTypes.end(), innerType) == allowedTypes.end()) {
        return;
    }

    if (genTable.find(SYMBOLS) == genTable.end() || !genTable[SYMBOLS].is<toml::Array>()) {
        return;
    }

    GenericTypeArguments typeArgs;
    auto symbolsArray = genTable[SYMBOLS].as<toml::Array>();

    for (const auto& symbol : symbolsArray) {
        if (!symbol.is<std::string>()) {
            continue;
        }

        typeArgs.symbols.insert(symbol.as<std::string>());
    }

    pkgConfig.allowedInteropCJGenericInstantiations[outerType][innerType] = std::move(typeArgs);
}

void ProcessNonGenericTypeWithSymbols(toml::Table& genTable, const std::string& name, PackageConfig& pkgConfig)
{
    if (genTable.find(SYMBOLS) == genTable.end() || !genTable[SYMBOLS].is<toml::Array>()) {
        return;
    }

    GenericTypeArguments typeArgs;
    auto symbolsArray = genTable[SYMBOLS].as<toml::Array>();

    for (const auto& symbol : symbolsArray) {
        if (!symbol.is<std::string>()) {
            continue;
        }

        typeArgs.symbols.insert(symbol.as<std::string>());
    }

    pkgConfig.allowedInteropCJGenericInstantiations[name][""] = std::move(typeArgs);
}

void ParseTupleConfiguration(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    if (packageTable.find(TUPLE_CONFIG) == packageTable.end() || !packageTable[TUPLE_CONFIG].is<toml::Array>()) {
        return;
    }

    auto tuples = packageTable[TUPLE_CONFIG].as<toml::Array>();

    for (const auto& item : tuples) {
        if (!item.is<std::string>()) {
            continue;
        }

        auto name = item.as<std::string>();
        pkgConfig.interopTuples.push_back(name);
    }
}

std::string Trim(const std::string& str)
{
    size_t start = 0;
    size_t end = str.length();

    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }

    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }

    return str.substr(start, end - start);
}

std::vector<std::string> ParseParameterList(const std::string& paramsStr)
{
    std::vector<std::string> parameters;
    if (paramsStr.empty()) {
        // such as "()->Int32"
        return parameters;
    }

    std::string currentParam;
    bool inGeneric = false; // whether in generic param, such as List<Int32>
    int genericDepth = 0;

    for (char ch : paramsStr) {
        if (ch == '<') {
            inGeneric = true;
            genericDepth++;
            currentParam += ch;
        } else if (ch == '>') {
            genericDepth--;
            if (genericDepth == 0) {
                inGeneric = false;
            }
            currentParam += ch;
        } else if (ch == ',' && !inGeneric) {
            std::string trimmedParam = Trim(currentParam);
            if (!trimmedParam.empty()) {
                parameters.push_back(trimmedParam);
            }
            currentParam.clear();
        } else {
            currentParam += ch;
        }
    }

    if (!currentParam.empty()) {
        std::string trimmedParam = Trim(currentParam);
        if (!trimmedParam.empty()) {
            parameters.push_back(trimmedParam);
        }
    }

    return parameters;
}

// Parse lambda signature, such as (Int32, Int64) -> Int32.
LambdaPattern ParseLambdaSignature(const std::string& signature)
{
    CJC_ASSERT(!signature.empty());

    std::string trimmed = Trim(signature);

    size_t arrowPos = trimmed.find("->");
    if (arrowPos == std::string::npos) {
        std::cerr << "Invalid lambda signature without ->." << std::endl;
    }
    if (trimmed.front() != '(' || trimmed.find(')') == std::string::npos) {
        std::cerr << "Invalid lambda signature without ()." << std::endl;
    }
    size_t closeParenPos = trimmed.find(')');
    if (closeParenPos >= arrowPos) {
        std::cerr << "Invalid lambda signature, -> position is not right." << std::endl;
    }

    std::string paramsStr = trimmed.substr(1, closeParenPos - 1);
    std::string returnTypeStr = trimmed.substr(arrowPos + 2);

    std::vector<std::string> paramTypes = ParseParameterList(paramsStr);

    std::string returnType = Trim(returnTypeStr);
    if (returnType.empty()) {
        std::cerr << "Invalid lambda signature, return type can not be empty." << std::endl;
    }

    return LambdaPattern(trimmed, paramTypes, returnType);
}

void ParseLmabdaPatternsConfiguration(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    if (packageTable.find(LAMBDA_PATTERNS) == packageTable.end() || !packageTable[LAMBDA_PATTERNS].is<Array>()) {
        return;
    }

    auto lambdaPatterns = packageTable[LAMBDA_PATTERNS].as<Array>();
    for (const auto& item : lambdaPatterns) {
        if (!item.is<Table>()) {
            continue;
        }

        auto genTable = item.as<Table>();
        // Check the name field.
        if (genTable.find(SIGNATURE) == genTable.end() || !genTable[SIGNATURE].is<std::string>()) {
            continue;
        }
        std::string lambdaSigna = genTable[SIGNATURE].as<std::string>();
        LambdaPattern lambdaPat = ParseLambdaSignature(lambdaSigna);
        // Check if it is a type parameter definition.
        if (genTable.find(CLASS_MAPPINGS) != genTable.end() && genTable[CLASS_MAPPINGS].is<Table>()) {
            auto classMappings = genTable[CLASS_MAPPINGS].as<Table>();

            for (const auto& [key, value] : classMappings) {
                ClassMapping classMap(key, value.as<std::string>());
                lambdaPat.ClassMappings.push_back(classMap);
            }
        }

        pkgConfig.lambdaPatterns.push_back(lambdaPat);
    }
}

// Helper function: Validates a comma-separated type string and returns the validated types
bool ValidateAndProcessTypeString(const std::string& typeString,
    const std::unordered_set<std::string>& validTypeSet,
    const PackageConfig& pkgConfig,
    std::vector<std::string>& validatedTypes)
{
    std::istringstream typeStream(typeString);
    std::string individualType;

    while (std::getline(typeStream, individualType, ',')) {
        // Remove whitespace from both ends
        auto firstChar = individualType.find_first_not_of(" \t\n\r");
        auto lastChar = individualType.find_last_not_of(" \t\n\r");

        if (firstChar == std::string::npos || lastChar == std::string::npos) {
            // Empty segment, skip
            continue;
        }

        individualType = individualType.substr(firstChar, lastChar - firstChar + 1);

        // Validate against allowed type set
        if (validTypeSet.find(individualType) == validTypeSet.end()) {
            std::cerr << "Error: Invalid type detected in package '"
                      << pkgConfig.name << "' configuration" << std::endl;
            std::cerr << "  Invalid type: '" << individualType << "'" << std::endl;
            std::cerr << "  Supported types (Exist difference between Java and Objc): ";

            bool first = true;
            for (const auto& validType : validTypeSet) {
                if (!first) std::cerr << ", ";
                std::cerr << validType;
                first = false;
            }
            std::cerr << std::endl;

            return false;  // Validation failed
        }

        validatedTypes.push_back(individualType);
    }

    return true;  // Validation successful
}

// Helper function: Combines a vector of type strings into a comma-separated string
std::string CombineTypesToString(const std::vector<std::string>& types)
{
    if (types.empty()) {
        return "";
    }

    std::ostringstream combinedStream;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) {
            combinedStream << ", ";
        }
        combinedStream << types[i];
    }

    return combinedStream.str();
}

// Helper function: Processes a TOML array of type parameters
bool ProcessTypeParameterArray(const toml::Array& typeArgs,
    const std::unordered_set<std::string>& validTypeSet,
    const PackageConfig& pkgConfig, std::vector<std::string>& collectedTypes,
    std::unordered_map<std::string, GenericTypeArguments>& genericInstantiations)
{
    for (const auto& type : typeArgs) {
        if (!type.is<std::string>()) {
            // Skip non-string type entries
            continue;
        }

        std::string typeString = type.as<std::string>();
        std::vector<std::string> validatedTypes;

        // Validate the type string
        if (!ValidateAndProcessTypeString(typeString, validTypeSet, pkgConfig, validatedTypes)) {
            return false; // Validation failed
        }

        if (validatedTypes.empty()) {
            std::cerr << "Error: Empty type definition in package '"
                      << pkgConfig.name << "'" << std::endl;
            std::cerr << "  Type string: '" << typeString << "'" << std::endl;
            return false;
        }

        // Combine validated types into a single string
        std::string combinedTypeString = CombineTypesToString(validatedTypes);
        collectedTypes.push_back(combinedTypeString);

        // Store with empty generic type arguments
        genericInstantiations[combinedTypeString] = GenericTypeArguments();
    }

    return true;  // Successfully processed all type parameters
}

bool CollectTypeArguments(toml::Array& allowedGenerics,
    std::unordered_map<std::string, std::vector<std::string>>& typeArgumentsMap,
    PackageConfig& pkgConfig)
{
    // Predefined set of valid Cangjie types.
    static const std::unordered_set<std::string> VALID_TYPE_SET = {
        "Int", "Int8", "Int16", "Int32", "Int64", "IntNative",
        "UInt8", "UInt16", "UInt32", "UInt64", "UIntNative",
        "Float16", "Float32", "Float64",
        "Bool", "Boolean", "Unit"
    };

    for (const auto& item : allowedGenerics) {
        if (!item.is<toml::Table>()) {
            // Skip non-table elements.
            continue;
        }

        auto genTable = item.as<toml::Table>();

        // Check for required package name field.
        if (genTable.find(PACKAGE_NAME) == genTable.end() ||
            !genTable[PACKAGE_NAME].is<std::string>()) {
            continue;  // Skip entries without valid package name
        }

        std::string packageName = genTable[PACKAGE_NAME].as<std::string>();

        // Verify type arguments array exists.
        if (genTable.find(TYPE_ARGUMENTS) == genTable.end() ||
            !genTable[TYPE_ARGUMENTS].is<toml::Array>()) {
            // Skip entries without type arguments.
            continue;
        }

        auto typeArgs = genTable[TYPE_ARGUMENTS].as<toml::Array>();
        std::vector<std::string> collectedTypes;

        // Process all type arguments in the array
        if (!ProcessTypeParameterArray(typeArgs, VALID_TYPE_SET, pkgConfig, collectedTypes,
                pkgConfig.allowedInteropCJGenericInstantiations[packageName])) {
            return false; // Validation failed
        }

        if (!collectedTypes.empty()) {
            typeArgumentsMap[packageName] = std::move(collectedTypes);
        }
    }

    // Successfully processed all type arguments.
    return true;
}

void ProcessSymbolConfigurations(toml::Array& allowedGenerics,
    const std::unordered_map<std::string, std::vector<std::string>>& typeArgumentsMap, PackageConfig& pkgConfig)
{
    for (const auto& item : allowedGenerics) {
        if (!item.is<toml::Table>()) {
            continue;
        }

        auto genTable = item.as<toml::Table>();

        if (genTable.find(PACKAGE_NAME) == genTable.end() || !genTable[PACKAGE_NAME].is<std::string>()) {
            continue;
        }

        std::string name = genTable[PACKAGE_NAME].as<std::string>();

        // Check if it's a generic type with angle brackets (e.g., "List<T>")
        size_t pos = name.find('<');
        if (pos != std::string::npos && name.back() == '>') {
            ProcessGenericTypeWithSymbols(genTable, name, pos, typeArgumentsMap, pkgConfig);
        } else if (genTable.find(SYMBOLS) != genTable.end() && genTable[SYMBOLS].is<toml::Array>()) {
            // Non-generic class with symbols
            ProcessNonGenericTypeWithSymbols(genTable, name, pkgConfig);
        }
    }
}

bool ParseGenericObjectConfiguration(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    if (packageTable.find(GENERIC_OBJECT_CONFIG) == packageTable.end() ||
        !packageTable[GENERIC_OBJECT_CONFIG].is<toml::Array>()) {
        return true;
    }

    auto allowedGenerics = packageTable[GENERIC_OBJECT_CONFIG].as<toml::Array>();

    // First pass: collect type parameter definitions
    std::unordered_map<std::string, std::vector<std::string>> typeArgumentsMap;
    bool trueHandled = CollectTypeArguments(allowedGenerics, typeArgumentsMap, pkgConfig);

    // Second pass: process symbol configurations
    ProcessSymbolConfigurations(allowedGenerics, typeArgumentsMap, pkgConfig);

    return trueHandled;
}

void ParseDefaultConfig(toml::Table& tbl, InteropCJPackageConfigReader& reader)
{
    if (tbl.find(DEFAULT_SECTION) == tbl.end()) {
        return;
    }

    const auto& defaultEntry = tbl.find(DEFAULT_SECTION)->second;
    if (!defaultEntry.is<toml::Table>()) {
        return;
    }

    auto defaultTable = defaultEntry.as<toml::Table>();
    if (defaultTable.find(API_STRATEGY) != defaultTable.end() && defaultTable[API_STRATEGY].is<std::string>()) {
        auto strategy = defaultTable[API_STRATEGY].as<std::string>();
        reader.defaultApiStrategy = StringToStrategy(strategy);
    }

    if (defaultTable.find(GENERIC_TYPE_STRATEGY) != defaultTable.end() &&
        defaultTable[GENERIC_TYPE_STRATEGY].is<std::string>()) {
        auto strategy = defaultTable[GENERIC_TYPE_STRATEGY].as<std::string>();
        reader.defaultGenericTypeStrategy = StringToGenericStrategy(strategy);
    }
}

bool ParseSinglePackage(toml::Table& packageTable, PackageConfig& pkgConfig)
{
    // Package name is required
    if (packageTable.find(PACKAGE_NAME) == packageTable.end() || !packageTable[PACKAGE_NAME].is<std::string>()) {
        return false;
    }

    pkgConfig.name = packageTable[PACKAGE_NAME].as<std::string>();

    // Parse API Strategy
    pkgConfig.apiStrategy = ParseAPIStrategy(packageTable);

    // Parse Generic Type Strategy
    pkgConfig.genericTypeStrategy = ParseGenericTypeStrategy(packageTable);

    // Parse included APIs
    ParseIncludedAPIs(packageTable, pkgConfig);

    // Parse excluded APIs
    ParseExcludedAPIs(packageTable, pkgConfig);

    // Parse generic object configuration
    bool trueHandled = ParseGenericObjectConfiguration(packageTable, pkgConfig);

    // Parse tuple configuration
    ParseTupleConfiguration(packageTable, pkgConfig);

    // Parse lambda patterns
    ParseLmabdaPatternsConfiguration(packageTable, pkgConfig);

    return trueHandled;
}

bool ParsePackageConfigurations(toml::Table& tbl, InteropCJPackageConfigReader& reader)
{
    if (tbl.find(PACKAGE_SECTION) == tbl.end()) {
        return true;
    }

    const auto& packageEntry = tbl.find(PACKAGE_SECTION)->second;
    if (!packageEntry.is<toml::Array>()) {
        return true;
    }

    auto packageArray = packageEntry.as<toml::Array>();

    for (const auto& packageItem : packageArray) {
        if (!packageItem.is<toml::Table>()) {
            continue;
        }

        PackageConfig pkgConfig;
        auto packageTable = packageItem.as<toml::Table>();
        if (!ParseSinglePackage(packageTable, pkgConfig)) {
            return false;
        }

        reader.packages[pkgConfig.name] = std::move(pkgConfig);
    }

    return true;
}

} // namespace

bool InteropCJPackageConfigReader::Parse(const std::string& filePath)
{
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open configuration file." << filePath << std::endl;
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
        file.close();

        // Check /* */
        if (content.find("/*") != std::string::npos) {
            std::cerr << "Failed to open configuration file. Error: Unsupported block comment '/*' detected." <<
                "Please use line comment '#' instead." << std::endl;
            return false;
        }

        toml::Table tbl = toml::parseFile(filePath).value.as<toml::Table>();

        ParseDefaultConfig(tbl, *this);

        bool trueHandled = ParsePackageConfigurations(tbl, *this);

        return trueHandled;
    } catch (const std::exception& e) {
        std::cerr << "Error parsing config: " << e.what() << std::endl;
        return false;
    }
}

std::optional<PackageConfig> InteropCJPackageConfigReader::GetPackage(const std::string& name) const
{
    auto it = packages.find(name);
    if (it != packages.end()) {
        return it->second;
    }
    return std::nullopt;
}

InteropCJStrategy InteropCJPackageConfigReader::GetApiStrategy(const std::string& packageName) const
{
    if (auto pkg = GetPackage(packageName)) {
        return pkg->apiStrategy;
    }
    return defaultApiStrategy;
}

InteropCJGenericStrategyType InteropCJPackageConfigReader::GetGenericTypeStrategy(const std::string& packageName) const
{
    if (auto pkg = GetPackage(packageName)) {
        return pkg->genericTypeStrategy;
    }
    return defaultGenericTypeStrategy;
}

bool InteropCJPackageConfigReader::Validate() const
{
    // Verifying Default Policies
    if (defaultApiStrategy == InteropCJStrategy::UNKNOWN) {
        std::cerr << "Validation failed: Default API strategy is unknown" << std::endl;
        return false;
    }
    if (defaultGenericTypeStrategy == InteropCJGenericStrategyType::UNKNOWN) {
        std::cerr << "Validation failed: Default generic type  strategy is unknown" << std::endl;
        return false;
    }

    // Verify each package
    for (const auto& [name, pkg] : packages) {
        // Verify policy value
        if (pkg.apiStrategy == InteropCJStrategy::UNKNOWN) {
            std::cerr << "Validation failed: '" << name << "' API strategy is unknown" << std::endl;
            return false;
        }

        if (pkg.genericTypeStrategy == InteropCJGenericStrategyType::UNKNOWN) {
            std::cerr << "Validation failed: '" << name << "' generic type strategy is unknown" << std::endl;
            return false;
        }

        // Verify the consistency between the validation strategy and the API list.
        if (pkg.apiStrategy == InteropCJStrategy::FULL && !pkg.interopCJIncludedApis.empty()) {
            std::cerr << "Validation failed for package '" << name
                      << "': API strategy is Full but IncludedApis is Configured " << std::endl;
            return false;
        }

        if (pkg.apiStrategy == InteropCJStrategy::NONE && !pkg.interopCJExcludedApis.empty()) {
            std::cerr << "Validation failed for package '" << name
                      << "': API strategy is None but ExcludedApis is Configured " << std::endl;
            return false;
        }

        if (!pkg.interopCJIncludedApis.empty() && !pkg.interopCJExcludedApis.empty()) {
            std::cerr << "Validation failed for package '" << name << "': Cannot hava both included and excluded APIs"
                      << std::endl;
            return false;
        }

        // Verify Generic Strategy Consistency
        if (pkg.genericTypeStrategy == InteropCJGenericStrategyType::NONE &&
            !pkg.allowedInteropCJGenericInstantiations.empty()) {
            // The "None" strategy does not allow for generic configurations.
            std::cerr << "Validation failed for package '" << name
                      << "': None generic strategy cannot hava generic instantiations" << std::endl;
            return false;
        }
    }
    return true;
}
} // namespace Cangjie