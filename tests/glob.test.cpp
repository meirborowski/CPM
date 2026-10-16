#include <fstream>

#include "core/glob.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

TEST(expands_braces) {
    CHECK_EQ(glob::expand_braces("a.{cpp,cc}"), (std::vector<std::string>{"a.cpp", "a.cc"}));
    CHECK_EQ(glob::expand_braces("{x,y}/{1,2}"), (std::vector<std::string>{"x/1", "x/2", "y/1", "y/2"}));
    CHECK_EQ(glob::expand_braces("a{b,{c,d}}e"), (std::vector<std::string>{"abe", "ace", "ade"}));
    CHECK_EQ(glob::expand_braces("plain"), (std::vector<std::string>{"plain"}));
}

TEST(matches_patterns) {
    CHECK(glob::match("src/*.cpp", "src/main.cpp"));
    CHECK(!glob::match("src/*.cpp", "src/a/main.cpp"));
    CHECK(glob::match("src/**/*.cpp", "src/main.cpp"));
    CHECK(glob::match("src/**/*.cpp", "src/a/b/main.cpp"));
    CHECK(!glob::match("src/**/*.cpp", "tests/main.cpp"));
    CHECK(glob::match("**", "a/b/c"));
    CHECK(glob::match("src/?.cpp", "src/a.cpp"));
    CHECK(!glob::match("src/?.cpp", "src/ab.cpp"));
    CHECK(glob::match("src/*.{cpp,cc}", "src/x.cc"));
    CHECK(!glob::match("src/*.cpp", "src/x.CPP"));  // case-sensitive
    CHECK(glob::match("a*b*c", "aXXbYYc"));
    CHECK(!glob::match("a*b*c", "aXXbYY"));
}

static void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

TEST(finds_files_in_order_with_excludes) {
    check::TempDir dir;
    for (const char* f : {"src/main.cpp", "src/b/z.cpp", "src/a.cc", "src/skip.cpp", "src/h.hpp",
                          "cpm_modules/x/src/evil.cpp", "src/cpm_modules/y.cpp"}) {
        touch(dir.path() / f);
    }
    auto found = glob::find(dir.path(), {"src/**/*.{cpp,cc}", "!src/skip.cpp"});
    CHECK_EQ(found, (std::vector<std::string>{"src/a.cc", "src/b/z.cpp", "src/main.cpp"}));
    CHECK_EQ(glob::find(dir.path(), {"src/main.cpp"}), (std::vector<std::string>{"src/main.cpp"}));
    CHECK(glob::find(dir.path(), {"nothing/**/*.cpp"}).empty());
}
