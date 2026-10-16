#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpm::semver {

// A SemVer 2.0.0 version (docs/spec/dependencies.md §2).
struct Version {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> pre;  // prerelease identifiers
    std::string build;             // build metadata (ignored for precedence)

    // Strict parse: no leading 'v', no leading zeros, no whitespace.
    static std::optional<Version> parse(std::string_view text);

    std::string to_string() const;
    bool is_prerelease() const noexcept { return !pre.empty(); }
};

// SemVer precedence (build metadata ignored).
std::strong_ordering compare(const Version& a, const Version& b);

inline bool operator==(const Version& a, const Version& b) { return compare(a, b) == 0; }
inline std::strong_ordering operator<=>(const Version& a, const Version& b) { return compare(a, b); }

}  // namespace cpm::semver
