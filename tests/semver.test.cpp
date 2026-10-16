#include <iterator>
#include <stdexcept>

#include "semver/version.hpp"
#include "support/check.hpp"

using cpm::semver::Version;

static Version v(const char* text) {
    auto parsed = Version::parse(text);
    if (!parsed) throw std::runtime_error(std::string("not a version: ") + text);
    return *parsed;
}

TEST(parses_valid_versions) {
    Version a = v("1.2.3");
    CHECK_EQ(a.major, 1u);
    CHECK_EQ(a.minor, 2u);
    CHECK_EQ(a.patch, 3u);
    CHECK(!a.is_prerelease());
    Version b = v("1.0.0-alpha.1+build.5");
    CHECK_EQ(b.pre.size(), 2u);
    CHECK_EQ(b.pre[1], "1");
    CHECK_EQ(b.build, "build.5");
    CHECK_EQ(b.to_string(), "1.0.0-alpha.1+build.5");
    CHECK_EQ(v("0.0.0").to_string(), "0.0.0");
}

TEST(rejects_invalid_versions) {
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "v1.2.3", "01.2.3", "1.02.3", "1.2.3-", "1.2.3-01",
                            "1.2.3+", "1.2.3-a..b", " 1.2.3", "1.2.3 ", "1.2.x", "-1.2.3"}) {
        if (Version::parse(bad)) ::check::fail(__FILE__, __LINE__, std::string("accepted: ") + bad);
    }
}

TEST(orders_by_semver_precedence) {
    // SemVer 2.0.0 §11 example chain.
    const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                           "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(chain); ++i) {
        if (!(v(chain[i]) < v(chain[i + 1])))
            ::check::fail(__FILE__, __LINE__, std::string(chain[i]) + " < " + chain[i + 1]);
    }
    CHECK(v("1.0.0+a") == v("1.0.0+b"));  // build metadata is ignored
    CHECK(v("10.0.0") > v("9.9.9"));
    CHECK(v("1.0.0-12345678901234567890") > v("1.0.0-9"));  // numeric identifiers beyond 64 bits
}
