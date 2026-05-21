// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <string>
#include <utility>
#include <vector>
#include "cangjie/Basic/StringConvertor.h"

#include "gtest/gtest.h"
using namespace Cangjie;

namespace {
unsigned char g_gbkCharArray[] = {176, 162, 203, 174};            // "阿水" in GBK encoding
unsigned char g_utf8CharArray[] = {233, 152, 191, 230, 176, 180}; // "阿水" in UTF-8 encoding
unsigned char g_errorCharArray[] = {129};                         // unknown encoding
unsigned char g_error2CharArray[] = {240, 169, 184};              // "𩸽" UTF-8 encoding missing last byte: 189

// Test data for IsGBK function bug
// GBK second byte valid range: 0x40-0xFE, excluding 0x7F
// Current IsGBK implementation bug: excludes 0xF7 instead of 0x7F
unsigned char g_gbkValidWith0xF7[] = {0x81, 0xF7};   // Valid GBK character (second byte is 0xF7)
unsigned char g_gbkInvalidWith0x7F[] = {0x81, 0x7F}; // Invalid GBK character (second byte is 0x7F)
unsigned char g_gbkMixedValid[] = {'H', 'e', 'l', 'l', 'o', 0x81, 0xF7, ' ', 'W', 'o', 'r', 'l', 'd'};
unsigned char g_gbkMixedInvalid[] = {'T', 'e', 's', 't', 0x81, 0x7F};
} // namespace

TEST(StringConvertorTest, GetStringEncoding)
{
    std::string gbkString(reinterpret_cast<const char*>(g_gbkCharArray), sizeof(g_gbkCharArray));
    EXPECT_EQ(StringConvertor::GetStringEncoding(gbkString), Cangjie::StringConvertor::GBK);
    std::string utf8String(reinterpret_cast<const char*>(g_utf8CharArray), sizeof(g_utf8CharArray));
    EXPECT_EQ(StringConvertor::GetStringEncoding(utf8String), Cangjie::StringConvertor::UTF8);
    std::string errorString(reinterpret_cast<const char*>(g_errorCharArray), sizeof(g_errorCharArray));
    EXPECT_EQ(StringConvertor::GetStringEncoding(errorString), Cangjie::StringConvertor::UNKNOWN);
    std::string error2String(reinterpret_cast<const char*>(g_error2CharArray), sizeof(g_error2CharArray));
    EXPECT_EQ(StringConvertor::GetStringEncoding(error2String), Cangjie::StringConvertor::UNKNOWN);
}

TEST(StringConvertorTest, GBKToUTF8)
{
    std::string gbkString(reinterpret_cast<const char*>(g_gbkCharArray), sizeof(g_gbkCharArray));
    std::string utf8String(reinterpret_cast<const char*>(g_utf8CharArray), sizeof(g_utf8CharArray));
    std::optional<std::string> str = StringConvertor::GBKToUTF8(gbkString);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), utf8String);
}

TEST(StringConvertorTest, UTF8ToGBK)
{
    std::string gbkString(reinterpret_cast<const char*>(g_gbkCharArray), sizeof(g_gbkCharArray));
    std::string utf8String(reinterpret_cast<const char*>(g_utf8CharArray), sizeof(g_utf8CharArray));
    std::optional<std::string> str = StringConvertor::UTF8ToGBK(utf8String);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), gbkString);
}

TEST(StringConvertorTest, NormalizeStringToUTF8)
{
    std::string gbkString(reinterpret_cast<const char*>(g_gbkCharArray), sizeof(g_gbkCharArray));
    std::string utf8String(reinterpret_cast<const char*>(g_utf8CharArray), sizeof(g_utf8CharArray));
    std::optional<std::string> str = StringConvertor::NormalizeStringToUTF8(gbkString);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), utf8String);

    str = StringConvertor::NormalizeStringToUTF8(utf8String);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), utf8String);
}

TEST(StringConvertorTest, NormalizeStringToGBK)
{
    std::string gbkString(reinterpret_cast<const char*>(g_gbkCharArray), sizeof(g_gbkCharArray));
    std::string utf8String(reinterpret_cast<const char*>(g_utf8CharArray), sizeof(g_utf8CharArray));
    std::optional<std::string> str = StringConvertor::NormalizeStringToGBK(gbkString);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), gbkString);

    str = StringConvertor::NormalizeStringToGBK(utf8String);
    EXPECT_EQ(str.has_value(), true);
    EXPECT_EQ(str.value(), gbkString);
}

TEST(StringConvertorTest, IsGBK)
{
    std::string validGBK(reinterpret_cast<const char*>(g_gbkValidWith0xF7), sizeof(g_gbkValidWith0xF7));
    std::string invalidGBK(reinterpret_cast<const char*>(g_gbkInvalidWith0x7F), sizeof(g_gbkInvalidWith0x7F));
    std::string mixedValid(reinterpret_cast<const char*>(g_gbkMixedValid), sizeof(g_gbkMixedValid));
    std::string mixedInvalid(reinterpret_cast<const char*>(g_gbkMixedInvalid), sizeof(g_gbkMixedInvalid));
    EXPECT_EQ(StringConvertor::GetStringEncoding(validGBK), Cangjie::StringConvertor::GBK);
    EXPECT_EQ(StringConvertor::GetStringEncoding(invalidGBK), Cangjie::StringConvertor::UNKNOWN);
    EXPECT_EQ(StringConvertor::GetStringEncoding(mixedValid), Cangjie::StringConvertor::GBK);
    EXPECT_EQ(StringConvertor::GetStringEncoding(mixedInvalid), Cangjie::StringConvertor::UNKNOWN);
}