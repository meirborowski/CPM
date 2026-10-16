#include <stdexcept>

#include "semver/range.hpp"
#include "support/check.hpp"

using cpm::semver::Range;
using cpm::semver::Version;

namespace {

Version v(const char* text) {
    auto parsed = Version::parse(text);
    if (!parsed) throw std::runtime_error(std::string("not a version: ") + text);
    return *parsed;
}

Range r(const char* text) {
    auto parsed = Range::parse(text);
    if (!parsed) throw std::runtime_error(std::string("not a range: ") + text);
    return *parsed;
}

// Checks every listed version against the range: those before "|" must match, those after must not.
void expect(const char* range, std::initializer_list<const char*> yes, std::initializer_list<const char*> no) {
    Range parsed = r(range);
    for (const char* y : yes) {
        if (!parsed.contains(v(y))) ::check::fail(__FILE__, __LINE__, std::string(range) + " should contain " + y);
    }
    for (const char* n : no) {
        if (parsed.contains(v(n))) ::check::fail(__FILE__, __LINE__, std::string(range) + " should not contain " + n);
    }
}

}  // namespace

TEST(caret) {
    expect("^1.2.3", {"1.2.3", "1.9.9", "1.2.4"}, {"1.2.2", "2.0.0", "2.0.0-alpha", "1.3.0-beta"});
    expect("^0.2.3", {"0.2.3", "0.2.99"}, {"0.3.0", "0.2.2"});
    expect("^0.0.3", {"0.0.3"}, {"0.0.4", "0.0.2"});
    expect("^0.0", {"0.0.0", "0.0.9"}, {"0.1.0"});
    expect("^0", {"0.0.0", "0.99.0"}, {"1.0.0"});
    expect("^1.2", {"1.2.0", "1.99.0"}, {"2.0.0", "1.1.9"});
    expect("^1.x", {"1.0.0", "1.5.5"}, {"2.0.0", "0.9.0"});
}

TEST(tilde_and_x_ranges) {
    expect("~1.2.3", {"1.2.3", "1.2.9"}, {"1.3.0", "1.2.2"});
    expect("~1.2", {"1.2.0", "1.2.9"}, {"1.3.0"});
    expect("~1", {"1.0.0", "1.9.0"}, {"2.0.0"});
    expect("1.2.x", {"1.2.0", "1.2.7"}, {"1.3.0"});
    expect("1", {"1.0.0", "1.9.9"}, {"2.0.0"});
    expect("*", {"0.0.0", "99.0.0"}, {"1.0.0-alpha"});
    expect("", {"1.2.3"}, {});
    expect("1.2.3", {"1.2.3", "1.2.3+build"}, {"1.2.4"});
    expect("=1.2.3", {"1.2.3"}, {"1.2.4"});
}

TEST(comparators_and_unions) {
    expect(">=1.2.3 <2", {"1.2.3", "1.99.0"}, {"2.0.0", "1.2.2", "2.0.0-0"});
    expect(">= 1.2.3 < 2", {"1.2.3"}, {"2.0.0"});  // operators separated by spaces
    expect(">1.2", {"1.3.0"}, {"1.2.9"});
    expect(">1.2.3", {"1.2.4"}, {"1.2.3"});
    expect("<=1.2", {"1.2.9"}, {"1.3.0"});
    expect("<1.2", {"1.1.9"}, {"1.2.0", "1.2.0-alpha"});
    expect("1.x || >=3.1", {"1.4.0", "3.1.0", "4.0.0"}, {"2.0.0", "3.0.0"});
    expect(">*", {}, {"1.0.0"});
}

TEST(hyphen_ranges) {
    expect("1.2.3 - 2.3", {"1.2.3", "2.3.9"}, {"2.4.0", "1.2.2"});
    expect("1.2 - 2.3.4", {"1.2.0", "2.3.4"}, {"2.3.5"});
}

TEST(prereleases) {
    expect("^1.3.0-beta.1", {"1.3.0-beta.1", "1.3.0-beta.2", "1.3.5"}, {"1.4.0-alpha", "1.3.0-alpha"});
    expect(">=1.0.0-rc.1 <1.0.1", {"1.0.0-rc.2", "1.0.0"}, {"1.0.1-rc.1"});
    expect("^1.2", {}, {"1.5.0-beta"});
}

TEST(rejects_bad_ranges) {
    for (const char* bad : {"^", ">=", "1.x.3", "01.2.3", "1.2.3.4", "abc", "^1.2.3-", "1.2 -", "1.2.3-01"}) {
        if (Range::parse(bad)) ::check::fail(__FILE__, __LINE__, std::string("accepted: ") + bad);
    }
}

TEST(intersection) {
    Range both = r("^1.2").intersect(r(">=1.5 <3"));
    CHECK(both.contains(v("1.5.0")));
    CHECK(both.contains(v("1.9.0")));
    CHECK(!both.contains(v("1.4.0")));
    CHECK(!both.contains(v("2.0.0")));
    CHECK(Range::any().contains(v("7.0.0")));
    CHECK(Range::exactly(v("1.0.0-rc.1")).contains(v("1.0.0-rc.1")));
}
