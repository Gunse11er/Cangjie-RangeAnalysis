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
 *
 */

#ifndef CANGJIE_BASIC_INTEROP_CJ_PACKAGECONFIGREADER_H
#define CANGJIE_BASIC_INTEROP_CJ_PACKAGECONFIGREADER_H

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Cangjie {
// Policy Type Enumeration
enum class InteropCJStrategy { FULL, NONE, UNKNOWN };

// InteropCJ Generic Strategy Type
enum class InteropCJGenericStrategyType { NONE, PARTIAL, UNKNOWN };

// Generic instanceization Structure
struct GenericTypeArguments {
    std::unordered_set<std::string> symbols;
};

// Custom Type Mapping Information
struct ClassMapping {
    std::string name;
    std::string pkgPath;
    ClassMapping(std::string name, std::string pkg) : name(name), pkgPath(pkg) {};
};

// lambda configuration pattern
struct LambdaPattern {
    std::string signature;
    std::vector<std::string> parameterTypes;
    std::string returnType;
    std::vector<ClassMapping> ClassMappings;
    std::string fullPackageName;
    LambdaPattern(std::string sign, std::vector<std::string> params, std::string retType)
        : signature(std::move(sign)), parameterTypes(std::move(params)), returnType(std::move(retType))
    {
    }
};
// Package Configuration Structure
struct PackageConfig {
    std::string name;
    InteropCJStrategy apiStrategy = InteropCJStrategy::UNKNOWN;
    InteropCJGenericStrategyType genericTypeStrategy = InteropCJGenericStrategyType::UNKNOWN;
    std::vector<std::string> interopCJIncludedApis;
    std::vector<std::string> interopCJExcludedApis;
    std::unordered_map<std::string, std::unordered_map<std::string, GenericTypeArguments>>
        allowedInteropCJGenericInstantiations;
    std::vector<std::string> interopTuples;
    std::vector<LambdaPattern> lambdaPatterns;
};

// Complete Configuration Structure
class InteropCJPackageConfigReader {
public:
    InteropCJStrategy defaultApiStrategy = InteropCJStrategy::NONE;
    InteropCJGenericStrategyType defaultGenericTypeStrategy = InteropCJGenericStrategyType::NONE;
    std::unordered_map<std::string, PackageConfig> packages;

    // Parsing configuration file
    bool Parse(const std::string& filePath);

    // Obtain package configuration
    std::optional<PackageConfig> GetPackage(const std::string& name) const;

    // Obtaining an API Policy
    InteropCJStrategy GetApiStrategy(const std::string& packageName) const;

    // Get Generic Policy
    InteropCJGenericStrategyType GetGenericTypeStrategy(const std::string& packageName) const;

    // Verify Configuration
    bool Validate() const;
};
} // namespace Cangjie
#endif // CANGJIE_BASIC_INTEROP_CJ_PACKAGECONFIGREADER_H