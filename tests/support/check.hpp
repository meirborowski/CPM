#pragma once

// CPM's tests run on doctest, a devDependency that CPM installs for itself (see "devDependencies" in
// cpm.jsonc). This header keeps the small vocabulary the tests are written in:
//   TEST(name) { ... }, CHECK(expr), CHECK_EQ(a, b), CHECK_THROWS(expr, "part of the message"),
//   check::fail(file, line, message) and check::TempDir.
// Each tests/*.test.cpp is its own executable: include this header once. Exit code 0 = all passed
// (docs/spec/build.md §7).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <random>
#include <string>

namespace check {

// Records a failure at file:line and lets the test case go on.
inline void fail(const char* file, int line, const std::string& message) { ADD_FAIL_CHECK_AT(file, line, message); }

// A fresh empty directory, removed when the object goes away.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / std::format("cpm-test-{:08x}{:08x}", rd(), rd());
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace check

#define TEST(name) TEST_CASE(#name)

// Any boolean expression, including && and || (doctest's own CHECK only decomposes simple comparisons).
#undef CHECK
#define CHECK(...) DOCTEST_CHECK((__VA_ARGS__))

// doctest's CHECK_EQ already prints both sides on failure.

// The exception's what() must contain `substring`.
#undef CHECK_THROWS
#define CHECK_THROWS(expr, substring) DOCTEST_CHECK_THROWS_WITH(expr, doctest::Contains(substring))
