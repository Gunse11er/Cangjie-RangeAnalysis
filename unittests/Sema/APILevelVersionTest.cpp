// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * Semantic version related unit tests for APILevel and IfAvailable annotations.
 */

#include "../../src/Sema/Plugin/APILevelVersion.h"
#include "../../src/Sema/Plugin/PluginCustomAnnoInfo.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

using namespace Cangjie;
using namespace Cangjie::PluginCheck;

// Test APILevelVersion construction
TEST(APILevelVersionTest, ConstructorTest)
{
    APILevelVersion v1;
    EXPECT_EQ(v1.major, 0);
    EXPECT_EQ(v1.minor, 0);
    EXPECT_EQ(v1.patch, 0);
    EXPECT_TRUE(v1.IsZero());

    APILevelVersion v2(20);
    EXPECT_EQ(v2.major, 20);
    EXPECT_EQ(v2.minor, 0);
    EXPECT_EQ(v2.patch, 0);
    EXPECT_FALSE(v2.IsZero());

    APILevelVersion v3(20, 1);
    EXPECT_EQ(v3.major, 20);
    EXPECT_EQ(v3.minor, 1);
    EXPECT_EQ(v3.patch, 0);

    APILevelVersion v4(20, 1, 5);
    EXPECT_EQ(v4.major, 20);
    EXPECT_EQ(v4.minor, 1);
    EXPECT_EQ(v4.patch, 5);
}

// Test parsing from string
TEST(APILevelVersionTest, ParseSimpleVersion)
{
    auto v = APILevelVersion::Parse("20");
    EXPECT_EQ(v.major, 20);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 0);
    EXPECT_EQ(v.ToString(), "20.0.0");
}

TEST(APILevelVersionTest, ParseTwoPartVersion)
{
    auto v = APILevelVersion::Parse("20.1");
    EXPECT_EQ(v.major, 20);
    EXPECT_EQ(v.minor, 1);
    EXPECT_EQ(v.patch, 0);
    EXPECT_EQ(v.ToString(), "20.1.0");
}

TEST(APILevelVersionTest, ParseFullVersion)
{
    auto v1 = APILevelVersion::Parse("20.0.0");
    EXPECT_EQ(v1.major, 20);
    EXPECT_EQ(v1.minor, 0);
    EXPECT_EQ(v1.patch, 0);
    EXPECT_EQ(v1.ToString(), "20.0.0");

    auto v2 = APILevelVersion::Parse("20.1.5");
    EXPECT_EQ(v2.major, 20);
    EXPECT_EQ(v2.minor, 1);
    EXPECT_EQ(v2.patch, 5);
    EXPECT_EQ(v2.ToString(), "20.1.5");

    auto v3 = APILevelVersion::Parse("21.10.999");
    EXPECT_EQ(v3.major, 21);
    EXPECT_EQ(v3.minor, 10);
    EXPECT_EQ(v3.patch, 999);
    EXPECT_EQ(v3.ToString(), "21.10.999");
}

// Test equality comparison
TEST(APILevelVersionTest, EqualityComparison)
{
    APILevelVersion v1(20, 0, 0);
    APILevelVersion v2(20, 0, 0);
    APILevelVersion v3(20, 1, 0);

    EXPECT_TRUE(v1 == v2);
    EXPECT_FALSE(v1 == v3);
    EXPECT_TRUE(v1 != v3);
    EXPECT_FALSE(v1 != v2);

    // Test that "20" parsed equals "20.0.0" parsed
    auto vSimple = APILevelVersion::Parse("20");
    auto vFull = APILevelVersion::Parse("20.0.0");
    EXPECT_TRUE(vSimple == vFull);
}

// Test less than comparison
TEST(APILevelVersionTest, LessThanComparison)
{
    APILevelVersion v1(19, 0, 0);
    APILevelVersion v2(20, 0, 0);
    APILevelVersion v3(20, 1, 0);
    APILevelVersion v4(20, 1, 5);
    APILevelVersion v5(21, 0, 0);

    // Major version comparison
    EXPECT_TRUE(v1 < v2);
    EXPECT_FALSE(v2 < v1);

    // Minor version comparison (same major)
    EXPECT_TRUE(v2 < v3);
    EXPECT_FALSE(v3 < v2);

    // Patch version comparison (same major and minor)
    EXPECT_TRUE(v3 < v4);
    EXPECT_FALSE(v4 < v3);

    // Mixed comparisons
    EXPECT_TRUE(v4 < v5);
    EXPECT_TRUE(v1 < v5);

    // Not less than itself
    EXPECT_FALSE(v2 < v2);
}

// Test greater than comparison
TEST(APILevelVersionTest, GreaterThanComparison)
{
    APILevelVersion v1(20, 0, 0);
    APILevelVersion v2(20, 1, 0);
    APILevelVersion v3(21, 0, 0);

    EXPECT_TRUE(v2 > v1);
    EXPECT_TRUE(v3 > v2);
    EXPECT_TRUE(v3 > v1);

    EXPECT_FALSE(v1 > v2);
    EXPECT_FALSE(v1 > v1);
}

// Test less than or equal comparison
TEST(APILevelVersionTest, LessThanOrEqualComparison)
{
    APILevelVersion v1(20, 0, 0);
    APILevelVersion v2(20, 0, 0);
    APILevelVersion v3(20, 1, 0);

    EXPECT_TRUE(v1 <= v2); // Equal
    EXPECT_TRUE(v1 <= v3); // Less than
    EXPECT_FALSE(v3 <= v1);
}

// Test greater than or equal comparison
TEST(APILevelVersionTest, GreaterThanOrEqualComparison)
{
    APILevelVersion v1(20, 0, 0);
    APILevelVersion v2(20, 0, 0);
    APILevelVersion v3(20, 1, 0);

    EXPECT_TRUE(v1 >= v2); // Equal
    EXPECT_TRUE(v3 >= v1); // Greater than
    EXPECT_FALSE(v1 >= v3);
}

// Test version ordering
TEST(APILevelVersionTest, VersionOrdering)
{
    std::vector<APILevelVersion> versions = {
        APILevelVersion::Parse("19.0.0"),
        APILevelVersion::Parse("20.0.0"),
        APILevelVersion::Parse("20.1.0"),
        APILevelVersion::Parse("20.1.5"),
        APILevelVersion::Parse("21.0.0"),
    };

    // Verify ordering
    for (size_t i = 0; i < versions.size() - 1; ++i) {
        EXPECT_TRUE(versions[i] < versions[i + 1])
            << versions[i].ToString() << " should be < " << versions[i + 1].ToString();
        EXPECT_FALSE(versions[i] > versions[i + 1]);
        EXPECT_FALSE(versions[i] == versions[i + 1]);
    }
}

// Test runtime encoding scheme
TEST(APILevelVersionTest, RuntimeEncoding)
{
    EXPECT_EQ(APILevelVersion::Parse("19.0.0").ToEncoded(), 19000000ULL);
    EXPECT_EQ(APILevelVersion::Parse("20.0.0").ToEncoded(), 20000000ULL);
    EXPECT_EQ(APILevelVersion::Parse("20.1.0").ToEncoded(), 20001000ULL);
    EXPECT_EQ(APILevelVersion::Parse("20.1.5").ToEncoded(), 20001005ULL);
    EXPECT_EQ(APILevelVersion::Parse("21.0.0").ToEncoded(), 21000000ULL);

    // Verify encoding maintains ordering
    uint64_t e1 = APILevelVersion::Parse("20.0.0").ToEncoded();
    uint64_t e2 = APILevelVersion::Parse("20.1.0").ToEncoded();
    uint64_t e3 = APILevelVersion::Parse("20.1.5").ToEncoded();
    uint64_t e4 = APILevelVersion::Parse("21.0.0").ToEncoded();

    EXPECT_LT(e1, e2);
    EXPECT_LT(e2, e3);
    EXPECT_LT(e3, e4);
}

// Test backward compatibility
TEST(APILevelVersionTest, BackwardCompatibility)
{
    // Integer literal treated as major version
    APILevelVersion vInt(20);
    auto vParsed = APILevelVersion::Parse("20");
    auto vFull = APILevelVersion::Parse("20.0.0");

    EXPECT_EQ(vInt, vParsed);
    EXPECT_EQ(vInt, vFull);
    EXPECT_EQ(vParsed, vFull);

    // All should have same encoding
    EXPECT_EQ(vInt.ToEncoded(), vParsed.ToEncoded());
    EXPECT_EQ(vInt.ToEncoded(), vFull.ToEncoded());
    EXPECT_EQ(vInt.ToEncoded(), 20000000ULL);
}

// Test IsZero method
TEST(APILevelVersionTest, IsZeroMethod)
{
    APILevelVersion v1;
    EXPECT_TRUE(v1.IsZero());

    APILevelVersion v2(0, 0, 0);
    EXPECT_TRUE(v2.IsZero());

    APILevelVersion v3(1, 0, 0);
    EXPECT_FALSE(v3.IsZero());

    APILevelVersion v4(0, 1, 0);
    EXPECT_FALSE(v4.IsZero());

    APILevelVersion v5(0, 0, 1);
    EXPECT_FALSE(v5.IsZero());
}

// Test ToString method
TEST(APILevelVersionTest, ToStringMethod)
{
    EXPECT_EQ(APILevelVersion(20, 0, 0).ToString(), "20.0.0");
    EXPECT_EQ(APILevelVersion(20, 1, 0).ToString(), "20.1.0");
    EXPECT_EQ(APILevelVersion(20, 1, 5).ToString(), "20.1.5");
    EXPECT_EQ(APILevelVersion(21, 10, 999).ToString(), "21.10.999");
}

// Test ToDisplayString: compact format for diagnostics (trims trailing zero components)
TEST(APILevelVersionTest, ToDisplayStringMethod)
{
    // Integer-style levels: only major is significant
    EXPECT_EQ(APILevelVersion(20, 0, 0).ToDisplayString(), "20");
    EXPECT_EQ(APILevelVersion(0, 0, 0).ToDisplayString(), "0");
    EXPECT_EQ(APILevelVersion(1, 0, 0).ToDisplayString(), "1");
    // Two-part: major.minor only
    EXPECT_EQ(APILevelVersion(20, 1, 0).ToDisplayString(), "20.1");
    EXPECT_EQ(APILevelVersion(0, 5, 0).ToDisplayString(), "0.5");
    // Full three-part
    EXPECT_EQ(APILevelVersion(20, 1, 5).ToDisplayString(), "20.1.5");
    EXPECT_EQ(APILevelVersion(21, 10, 999).ToDisplayString(), "21.10.999");
    // ToString always shows all three components regardless
    EXPECT_NE(APILevelVersion(20, 0, 0).ToString(), APILevelVersion(20, 0, 0).ToDisplayString());
}

// Test edge cases
TEST(APILevelVersionTest, EdgeCases)
{
    // Empty string
    auto v1 = APILevelVersion::Parse("");
    EXPECT_TRUE(v1.IsZero());

    // Single zero
    auto v2 = APILevelVersion::Parse("0");
    EXPECT_EQ(v2.major, 0);
    EXPECT_EQ(v2.minor, 0);
    EXPECT_EQ(v2.patch, 0);

    // Large numbers
    auto v3 = APILevelVersion::Parse("999.999.999");
    EXPECT_EQ(v3.major, 999);
    EXPECT_EQ(v3.minor, 999);
    EXPECT_EQ(v3.patch, 999);

    // Trailing dots (should be handled gracefully)
    auto v4 = APILevelVersion::Parse("20.");
    EXPECT_EQ(v4.major, 20);
    EXPECT_EQ(v4.minor, 0);
    EXPECT_EQ(v4.patch, 0);
}

// Test IsValidFormat method
TEST(APILevelVersionTest, IsValidFormat)
{
    // Valid formats
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20"));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20.1"));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20.1.5"));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("0.0.0"));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("999.999.999"));

    // Invalid formats
    EXPECT_FALSE(APILevelVersion::IsValidFormat(""));         // empty
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20."));      // trailing dot
    EXPECT_FALSE(APILevelVersion::IsValidFormat(".20"));      // leading dot
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20..1"));    // consecutive dots
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.abc.1")); // non-numeric component
    // more than 3 dotted components rejected
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.500.600.700"));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.1.2a"));  // alphanumeric
}

TEST(APILevelVersionTest, IsValidFormatWithStrictRules)
{
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20", APILevelVersion::ParseRule::MAJOR_ONLY));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20.1.5", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE));
    EXPECT_TRUE(APILevelVersion::IsValidFormat("20.1.5", APILevelVersion::ParseRule::TRIPLE_ONLY));

    EXPECT_FALSE(APILevelVersion::IsValidFormat("0", APILevelVersion::ParseRule::MAJOR_ONLY));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.1", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.1", APILevelVersion::ParseRule::TRIPLE_ONLY));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.01.5", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("100", APILevelVersion::ParseRule::MAJOR_ONLY));
    EXPECT_FALSE(APILevelVersion::IsValidFormat("20.100.5", APILevelVersion::ParseRule::TRIPLE_ONLY));
}

TEST(APILevelVersionTest, ParseCheckedWithStrictRules)
{
    auto single = APILevelVersion::ParseChecked("26", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE);
    ASSERT_TRUE(single.has_value());
    EXPECT_EQ(*single, APILevelVersion(26, 0, 0));

    auto triple = APILevelVersion::ParseChecked("26.0.1", APILevelVersion::ParseRule::TRIPLE_ONLY);
    ASSERT_TRUE(triple.has_value());
    EXPECT_EQ(*triple, APILevelVersion(26, 0, 1));

    EXPECT_FALSE(APILevelVersion::ParseChecked("26.0", APILevelVersion::ParseRule::MAJOR_OR_TRIPLE).has_value());
    EXPECT_FALSE(APILevelVersion::ParseChecked("26.00.1", APILevelVersion::ParseRule::TRIPLE_ONLY).has_value());
    EXPECT_FALSE(APILevelVersion::ParseChecked("0", APILevelVersion::ParseRule::MAJOR_ONLY).has_value());
}

// Test PluginCustomAnnoInfo structure
TEST(APILevelVersionTest, PluginCustomAnnoInfoIntegration)
{
    PluginCustomAnnoInfo info1;
    EXPECT_TRUE(info1.since.IsZero());

    PluginCustomAnnoInfo info2;
    info2.since = APILevelVersion(20, 1, 5);
    EXPECT_FALSE(info2.since.IsZero());
    EXPECT_EQ(info2.since.ToString(), "20.1.5");

    // Test comparison
    PluginCustomAnnoInfo info3;
    info3.since = APILevelVersion(21, 0, 0);
    EXPECT_TRUE(info2.since < info3.since);
}

// Test real-world version scenarios
TEST(APILevelVersionTest, RealWorldScenarios)
{
    // Integer API level style (major version only)
    auto api19 = APILevelVersion::Parse("19");
    auto api20 = APILevelVersion::Parse("20");
    auto api21 = APILevelVersion::Parse("21");

    EXPECT_TRUE(api19 < api20);
    EXPECT_TRUE(api20 < api21);

    // x.y.z style versioning
    auto harmony400 = APILevelVersion::Parse("4.0.0");
    auto harmony410 = APILevelVersion::Parse("4.1.0");
    auto harmony500 = APILevelVersion::Parse("5.0.0");

    EXPECT_TRUE(harmony400 < harmony410);
    EXPECT_TRUE(harmony410 < harmony500);

    // Patch version updates
    auto v100 = APILevelVersion::Parse("1.0.0");
    auto v101 = APILevelVersion::Parse("1.0.1");
    auto v1010 = APILevelVersion::Parse("1.0.10");

    EXPECT_TRUE(v100 < v101);
    EXPECT_TRUE(v101 < v1010);
}

// ============================================================
// Tests for --cfg APILevel scenario
//
// Simulates the CheckLevel logic triggered by --cfg APILevel_level=X.Y.Z:
//   - globalLevel  : parsed from --cfg APILevel_level (i.e., scopeLevel when no IfAvailable)
//   - targetSince  : from the @APILevel(since: "X.Y.Z") annotation on the referenced API
//   - Rule: if targetSince > scopeLevel → inaccessible (should report error)
//           if targetSince <= scopeLevel → accessible (no error)
// ============================================================

namespace {
// Helper: simulate CheckLevel return value.
// Returns true when the API is accessible (no error should be reported),
// false when the API level is too high (error should be reported).
bool IsAccessible(const APILevelVersion& targetSince, const APILevelVersion& scopeLevel)
{
    // Mirrors PluginCustomAnnoChecker::CheckLevel: error when targetSince > scopeLevel
    return !(targetSince > scopeLevel);
}
} // namespace

// --cfg APILevel_level=20 (integer style)
TEST(CfgAPILevelTest, IntegerStyleCfg_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20"); // --cfg APILevel_level=20
    auto targetSince = APILevelVersion::Parse("20"); // @APILevel(since: "20")
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, IntegerStyleCfg_Inaccessible)
{
    auto globalLevel = APILevelVersion::Parse("19"); // --cfg APILevel_level=19
    auto targetSince = APILevelVersion::Parse("20"); // @APILevel(since: "20")
    EXPECT_FALSE(IsAccessible(targetSince, globalLevel));
}

// --cfg APILevel_level=20.0.0 (full x.y.z style)
TEST(CfgAPILevelTest, FullVersionCfg_ExactMatch_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20.0.0");
    auto targetSince = APILevelVersion::Parse("20.0.0");
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, FullVersionCfg_HigherTarget_Inaccessible)
{
    auto globalLevel = APILevelVersion::Parse("20.0.0"); // --cfg APILevel_level=20.0.0
    auto targetSince = APILevelVersion::Parse("20.1.0"); // @APILevel(since: "20.1.0")
    EXPECT_FALSE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, FullVersionCfg_LowerTarget_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1.0"); // --cfg APILevel_level=20.1.0
    auto targetSince = APILevelVersion::Parse("20.0.0"); // @APILevel(since: "20.0.0")
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

// Patch-level boundary
TEST(CfgAPILevelTest, PatchBoundary_ExactMatch_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1.5");
    auto targetSince = APILevelVersion::Parse("20.1.5");
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, PatchBoundary_OneHigher_Inaccessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1.5"); // --cfg APILevel_level=20.1.5
    auto targetSince = APILevelVersion::Parse("20.1.6"); // @APILevel(since: "20.1.6")
    EXPECT_FALSE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, PatchBoundary_OneLower_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1.5"); // --cfg APILevel_level=20.1.5
    auto targetSince = APILevelVersion::Parse("20.1.4"); // @APILevel(since: "20.1.4")
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

// Integer --cfg is equivalent to x.0.0
TEST(CfgAPILevelTest, IntegerEquivalentToMajorZeroZero)
{
    auto cfgInt = APILevelVersion::Parse("20");      // --cfg APILevel_level=20
    auto cfgFull = APILevelVersion::Parse("20.0.0"); // --cfg APILevel_level=20.0.0

    // Both forms must yield the same API availability decisions
    auto targetBelow = APILevelVersion::Parse("19.9.9");
    auto targetEqual = APILevelVersion::Parse("20.0.0");
    auto targetAbove = APILevelVersion::Parse("20.0.1");

    EXPECT_EQ(IsAccessible(targetBelow, cfgInt), IsAccessible(targetBelow, cfgFull));
    EXPECT_EQ(IsAccessible(targetEqual, cfgInt), IsAccessible(targetEqual, cfgFull));
    EXPECT_EQ(IsAccessible(targetAbove, cfgInt), IsAccessible(targetAbove, cfgFull));
}

// Two-part --cfg (e.g., --cfg APILevel_level=20.1)
TEST(CfgAPILevelTest, TwoPartCfg_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1");   // --cfg APILevel_level=20.1
    auto targetSince = APILevelVersion::Parse("20.1.0"); // @APILevel(since: "20.1.0")
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, TwoPartCfg_Inaccessible)
{
    auto globalLevel = APILevelVersion::Parse("20.1");   // --cfg APILevel_level=20.1
    auto targetSince = APILevelVersion::Parse("20.2.0"); // @APILevel(since: "20.2.0")
    EXPECT_FALSE(IsAccessible(targetSince, globalLevel));
}

// Cross-major boundary
TEST(CfgAPILevelTest, CrossMajor_OlderCfg_Inaccessible)
{
    auto globalLevel = APILevelVersion::Parse("20.9.9"); // --cfg APILevel_level=20.9.9
    auto targetSince = APILevelVersion::Parse("21.0.0"); // @APILevel(since: "21.0.0")
    EXPECT_FALSE(IsAccessible(targetSince, globalLevel));
}

TEST(CfgAPILevelTest, CrossMajor_NewerCfg_Accessible)
{
    auto globalLevel = APILevelVersion::Parse("21.0.0"); // --cfg APILevel_level=21.0.0
    auto targetSince = APILevelVersion::Parse("20.9.9"); // @APILevel(since: "20.9.9")
    EXPECT_TRUE(IsAccessible(targetSince, globalLevel));
}

// No --cfg provided: globalLevel is zero, optionWithLevel==false (CheckLevel returns true immediately).
// Modeled here as: zero globalLevel should never block access via the IsZero guard.
TEST(CfgAPILevelTest, NoCfgProvided_GlobalLevelIsZero)
{
    APILevelVersion globalLevel; // default-constructed = 0.0.0 (IsZero)
    EXPECT_TRUE(globalLevel.IsZero());
    // When optionWithLevel is false, CheckLevel returns true regardless; ensure zero is detectable.
    auto targetSince = APILevelVersion::Parse("21.0.0");
    // The real CheckLevel would skip the check; here we verify the zero sentinel.
    EXPECT_FALSE(targetSince.IsZero());
}

// Batch API availability: multiple APIs at different levels with a fixed --cfg
TEST(CfgAPILevelTest, BatchAccessibility_FixedCfg)
{
    auto globalLevel = APILevelVersion::Parse("20.1.5"); // --cfg APILevel_level=20.1.5

    // accessible APIs (since <= 20.1.5)
    EXPECT_TRUE(IsAccessible(APILevelVersion::Parse("19.0.0"), globalLevel));
    EXPECT_TRUE(IsAccessible(APILevelVersion::Parse("20.0.0"), globalLevel));
    EXPECT_TRUE(IsAccessible(APILevelVersion::Parse("20.1.0"), globalLevel));
    EXPECT_TRUE(IsAccessible(APILevelVersion::Parse("20.1.5"), globalLevel));

    // inaccessible APIs (since > 20.1.5)
    EXPECT_FALSE(IsAccessible(APILevelVersion::Parse("20.1.6"), globalLevel));
    EXPECT_FALSE(IsAccessible(APILevelVersion::Parse("20.2.0"), globalLevel));
    EXPECT_FALSE(IsAccessible(APILevelVersion::Parse("21.0.0"), globalLevel));
}
