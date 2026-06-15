// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the APILevelVersion type used for @!APILevel and @IfAvailable checks.
 *
 * API level version encoding for runtime comparison:
 *   encoded = major * 1_000_000 + minor * 1_000 + patch
 *
 * Strict validation used by APILevel/@IfAvailable parsing:
 *   major : 1 – 99
 *   minor : 0 – 99
 *   patch : 0 – 99
 */

#ifndef CANGJIE_BASIC_APILEVELVERSION_H
#define CANGJIE_BASIC_APILEVELVERSION_H

#include <cstdint>
#include <optional>
#include <string>

// Linux glibc <sys/sysmacros.h> defines major()/minor() as device-id
// macros and pulls them in transitively via <sys/types.h>. Undef them
// here so our struct members named major/minor are not macro-expanded.
// (Windows builds cross-compile on Linux and inherit the same headers.)
#ifdef major
#undef major
#endif
#ifdef minor
#undef minor
#endif

namespace Cangjie {

/**
 * @brief API level version triple (major.minor.patch).
 *
 * Used for @!APILevel since values and --cfg APILevel_level option.
 * The sentinel value 0.0.0 means "not set" (check IsZero()).
 */
struct APILevelVersion {
    enum class ParseRule {
        MAJOR_ONLY,
        MAJOR_OR_TRIPLE,
        TRIPLE_ONLY,
    };

    uint32_t major{0};
    uint32_t minor{0};
    uint32_t patch{0};

    APILevelVersion() = default;
    explicit APILevelVersion(uint32_t maj, uint32_t min = 0, uint32_t pat = 0) : major(maj), minor(min), patch(pat)
    {
    }

    /// Returns true when the version is the default-constructed sentinel 0.0.0.
    bool IsZero() const
    {
        return major == 0 && minor == 0 && patch == 0;
    }

    /// Returns "major.minor.patch" string representation.
    std::string ToString() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    /**
     * @brief Returns a compact display string, omitting trailing zero components.
     *
     * This preserves backward compatibility with integer-style API levels:
     *   {10, 0, 0} -> "10"
     *   {10, 1, 0} -> "10.1"
     *   {10, 1, 5} -> "10.1.5"
     * Used for diagnostic messages so existing test golden output is unchanged.
     */
    std::string ToDisplayString() const;

    /**
     * @brief Encodes the version as a single uint64_t suitable for runtime comparison.
     *
     * Encoding: major * 1_000_000 + minor * 1_000 + patch
     * This ordering guarantees that encoded(v1) < encoded(v2) iff v1 < v2,
     * provided minor and patch are each in [0, 999].
     */
    uint64_t ToEncoded() const
    {
        return static_cast<uint64_t>(major) * 1000000ULL + static_cast<uint64_t>(minor) * 1000ULL +
            static_cast<uint64_t>(patch);
    }

    /**
     * @brief Parses a version string of the form "major[.minor[.patch]]".
     *
     * Accepts "20", "20.1", "20.1.5". Non-numeric components default to 0.
     * Returns the zero version APILevelVersion{0,0,0} for empty or entirely
     * invalid input.
     */
    static APILevelVersion Parse(const std::string& s);

    /// Strict parser for current annotation/config validation rules.
    static std::optional<APILevelVersion> ParseChecked(const std::string& s, ParseRule rule);

    /**
     * @brief Validates that @p s is a well-formed version string.
     *
     * A valid string contains 1–3 dot-separated components, each consisting
     * solely of decimal digits.
     */
    static bool IsValidFormat(const std::string& s);

    static bool IsValidFormat(const std::string& s, ParseRule rule);

    bool operator<(const APILevelVersion& other) const
    {
        if (major != other.major) {
            return major < other.major;
        }
        if (minor != other.minor) {
            return minor < other.minor;
        }
        return patch < other.patch;
    }

    bool operator<=(const APILevelVersion& other) const
    {
        return *this < other || *this == other;
    }

    bool operator>(const APILevelVersion& other) const
    {
        return other < *this;
    }

    bool operator>=(const APILevelVersion& other) const
    {
        return !(*this < other);
    }

    bool operator==(const APILevelVersion& other) const
    {
        return major == other.major && minor == other.minor && patch == other.patch;
    }

    bool operator!=(const APILevelVersion& other) const
    {
        return !(*this == other);
    }
};

} // namespace Cangjie

#endif // CANGJIE_BASIC_APILEVELVERSION_H
