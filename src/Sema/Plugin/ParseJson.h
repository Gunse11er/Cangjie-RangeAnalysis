// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares json parsing functions.
 */

#ifndef PARSE_JSON_H
#define PARSE_JSON_H

#include <cstdint>
#include <string>
#include <vector>

#include "cangjie/Utils/SafePointer.h"

namespace Cangjie {
namespace PluginCheck {

struct JsonObject;

/**
 * @brief Represents a key-value pair in a JSON object.
 */
struct JsonPair {
    std::string key;
    std::vector<std::string> valueStr;
    std::vector<OwnedPtr<JsonObject>> valueObj;
    std::vector<uint64_t> valueNum;
};

/**
 * @brief Represents a JSON object containing multiple key-value pairs.
 */
struct JsonObject {
    std::vector<OwnedPtr<JsonPair>> pairs;
};

/**
 * @brief Enum to indicate whether we are parsing a key or a value in JSON.
 */
enum class StringMod {
    KEY,
    VALUE,
};

/**
 * @brief Parse a json string from input data.
 * @param pos Current position in input data.
 * @param in Json data.
 * @return Parsed JsonObject.
 */
OwnedPtr<JsonObject> ParseJsonObject(size_t& pos, const std::vector<uint8_t>& in);

/**
 * @brief Get json string values by key from a JsonObject.
 * @param root Root JsonObject.
 * @param key Key to search for.
 * @return Vector of string values associated with the key.
 */
std::vector<std::string> GetJsonString(Ptr<JsonObject> root, const std::string& key);

/**
 * @brief Get json object values by key from a JsonObject.
 * @param root Root JsonObject.
 * @param key Key to search for.
 * @param index Index of the object in the valueObj vector.
 * @return Ptr to the JsonObject associated with the key and index.
 */
Ptr<JsonObject> GetJsonObject(Ptr<JsonObject> root, const std::string& key, const size_t index);
} // namespace PluginCheck
} // namespace Cangjie
#endif