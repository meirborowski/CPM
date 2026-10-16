#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "semver/version.hpp"

namespace cpm::semver {

// An npm-style version range (docs/spec/dependencies.md §3): a union of comparator sets, each stored as
// one interval of version precedence plus the MAJOR.MINOR.PATCH tuples for which prereleases may match.
class Range {
public:
    // Parses `^1.2`, `~1.2.3`, `>=1 <2`, `1.x || 2.0.0 - 2.3`, `*`, ... Returns nullopt on a syntax error.
    static std::optional<Range> parse(std::string_view text);
    static Range any();                       // "*": every non-prerelease version
    static Range exactly(const Version& v);  // "=v"

    bool contains(const Version& v) const;
    Range intersect(const Range& other) const;  // versions in both ranges

    const std::string& text() const { return text_; }  // as written (or a synthesized description)

private:
    struct Bound {
        Version version;
        bool inclusive = true;
    };
    struct Interval {
        std::optional<Bound> lo;  // nullopt: unbounded
        std::optional<Bound> hi;
        std::vector<std::array<std::uint64_t, 3>> prerelease_ok;
        bool nothing = false;  // matches no version at all (e.g. ">*")
    };

    static bool in(const Interval& i, const Version& v);
    static Interval intersect(const Interval& a, const Interval& b);

    std::vector<Interval> sets_;
    std::string text_;
};

}  // namespace cpm::semver
