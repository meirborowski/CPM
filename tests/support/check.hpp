#pragma once

// CPM's tests run on doctest, a devDependency that CPM installs for itself (see "devDependencies" in
// cpm.jsonc). This header keeps the small vocabulary the tests are written in:
//   TEST(name) { ... }, CHECK(expr), CHECK_EQ(a, b), CHECK_THROWS(expr, "part of the message"),
//   check::fail(file, line, message) and check::TempDir.
// Each tests/*.test.cpp is its own executable: include this header once. Exit code 0 = all passed
// (docs/spec/build.md §7).
// On macOS doctest's implementation includes <sys/sysctl.h>, which declares a global `struct proc`. Next to
// `using namespace cpm;` a plain `proc::` is then ambiguous, so tests spell out `cpm::proc::`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <thread>

#include "core/fsx.hpp"
#include "core/paths.hpp"

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
    // Deletes with cpm::fsx::remove_tree rather than std::filesystem::remove_all, which on Windows stops at the
    // read-only files git makes for its objects, and with libstdc++ may follow the junctions in cpm_modules.
    // A virus scanner can hold a new file open for a moment, so it tries for about a second.
    ~TempDir() {
        for (int attempt = 0;; ++attempt) {
            try {
                cpm::fsx::remove_tree(path_);
                return;
            } catch (const std::exception& e) {
                if (attempt == 10) {
                    std::fprintf(stderr, "warning: cannot remove %s: %s\n", cpm::paths::to_utf8(path_).c_str(), e.what());
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
        }
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
