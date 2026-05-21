// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "../src/Sema/Plugin/ParseJson.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

using namespace Cangjie;
using namespace PluginCheck;

class ParseJsonTest : public testing::Test {
protected:
    std::vector<uint8_t> ToBytes(const std::string& str)
    {
        return std::vector<uint8_t>(str.begin(), str.end());
    }
};

TEST_F(ParseJsonTest, ParseEmptyObject)
{
    std::string json = "{}";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->pairs.size(), 0u);
}

TEST_F(ParseJsonTest, ParseSimpleString)
{
    std::string json = R"({"key": "value"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->key, "key");
    ASSERT_EQ(obj->pairs[0]->valueStr.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "value");
}

TEST_F(ParseJsonTest, ParseMultipleKeys)
{
    std::string json = R"({"key1": "value1", "key2": "value2"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 2u);
    EXPECT_EQ(obj->pairs[0]->key, "key1");
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "value1");
    EXPECT_EQ(obj->pairs[1]->key, "key2");
    EXPECT_EQ(obj->pairs[1]->valueStr[0], "value2");
}

TEST_F(ParseJsonTest, ParseNumber)
{
    std::string json = R"({"count": 42})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->key, "count");
    ASSERT_EQ(obj->pairs[0]->valueNum.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueNum[0], 42u);
}

TEST_F(ParseJsonTest, ParseNestedObject)
{
    std::string json = R"({"outer": {"inner": "value"}})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->key, "outer");
    ASSERT_EQ(obj->pairs[0]->valueObj.size(), 1u);
    auto innerObj = obj->pairs[0]->valueObj[0].get();
    ASSERT_NE(innerObj, nullptr);
    ASSERT_EQ(innerObj->pairs.size(), 1u);
    EXPECT_EQ(innerObj->pairs[0]->key, "inner");
    EXPECT_EQ(innerObj->pairs[0]->valueStr[0], "value");
}

TEST_F(ParseJsonTest, ParseArray)
{
    std::string json = R"({"items": ["a", "b", "c"]})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->key, "items");
    ASSERT_EQ(obj->pairs[0]->valueStr.size(), 3u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "a");
    EXPECT_EQ(obj->pairs[0]->valueStr[1], "b");
    EXPECT_EQ(obj->pairs[0]->valueStr[2], "c");
}

TEST_F(ParseJsonTest, ParseEscapedDoubleQuotes)
{
    std::string json = R"({"key": "value\"with\"quotes"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "value\"with\"quotes");
}

TEST_F(ParseJsonTest, ParseEscapedBackslash)
{
    std::string json = R"({"path": "C:\\Users\\test"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "C:\\Users\\test");
}

TEST_F(ParseJsonTest, ParseEscapedNewline)
{
    std::string json = R"({"text": "line1\nline2"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "line1\nline2");
}

TEST_F(ParseJsonTest, ParseEscapedTab)
{
    std::string json = R"({"text": "col1\tcol2"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "col1\tcol2");
}

TEST_F(ParseJsonTest, ParseComplexEscapedString)
{
    std::string json = R"({"key": "a\"b\\c\nd\te"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 1u);
    EXPECT_EQ(obj->pairs[0]->valueStr[0], "a\"b\\c\nd\te");
}

TEST_F(ParseJsonTest, GetJsonStringTest)
{
    std::string json = R"({"name": "test", "value": "123"})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    
    auto values = GetJsonString(obj.get(), "name");
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], "test");
    
    values = GetJsonString(obj.get(), "value");
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], "123");
    
    values = GetJsonString(obj.get(), "notexist");
    EXPECT_EQ(values.size(), 0u);
}

TEST_F(ParseJsonTest, GetJsonObjectTest)
{
    std::string json = R"({"config": {"setting": "enabled"}})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    
    auto innerObj = GetJsonObject(obj.get(), "config", 0);
    ASSERT_NE(innerObj, nullptr);
    ASSERT_EQ(innerObj->pairs.size(), 1u);
    EXPECT_EQ(innerObj->pairs[0]->key, "setting");
    EXPECT_EQ(innerObj->pairs[0]->valueStr[0], "enabled");
}

TEST_F(ParseJsonTest, ParseWhitespaceHandling)
{
    std::string json = R"({
        "key1": "value1",
        "key2": 123
    })";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    ASSERT_EQ(obj->pairs.size(), 2u);
    EXPECT_EQ(obj->pairs[0]->key, "key1");
    EXPECT_EQ(obj->pairs[1]->key, "key2");
}

TEST_F(ParseJsonTest, ParseNumberWithoutColon)
{
    std::string json = R"({123})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->pairs.size(), 0u);
}

TEST_F(ParseJsonTest, ParseObjectWithoutColon)
{
    std::string json = R"({{"inner": "value"}})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->pairs.size(), 0u);
}

TEST_F(ParseJsonTest, ParseArrayWithoutColon)
{
    std::string json = R"({["a", "b"]})";
    auto bytes = ToBytes(json);
    size_t pos = 0;
    auto obj = ParseJsonObject(pos, bytes);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->pairs.size(), 0u);
}
