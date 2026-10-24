#pragma once

// CPM's tests run on cpm-test (https://github.com/cpm-org/cpm-test), a devDependency that CPM installs for itself (see
// "devDependencies" in cpm.jsonc). This header keeps the small vocabulary the tests are written in:
//   TEST(name) { ... }, CHECK(expr), CHECK_EQ(a, b), CHECK_THROWS(expr, "part of the message"), INFO(format, args...),
//   check::fail(file, line, message) and check::TempDir.
// CHECK shows both values of a failed comparison. Like cpm-test's own CHECK, it takes no unparenthesized && or ||:
// write CHECK((a && b)), or two CHECKs. Each tests/*.test.cpp is its own executable, and cpm-test supplies its main().
// Exit code 0 = all passed (docs/spec/build.md §7).

#define CPM_TEST_NO_SHORT_MACROS  // this header defines the names the tests use
#include <cpm/test.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "core/fsx.hpp"
#include "core/paths.hpp"

namespace check {

// Records a failure at file:line and lets the test case go on.
inline void fail(const char* file, int line, const std::string& message) {
    ::cpm::test::detail::fail_and_continue(file, line, message);
}

// What `fn` threw: the message of a std::exception or of a thrown string. nullopt when it threw nothing.
template <typename Fn>
std::optional<std::string> thrown_message(Fn&& fn) {
    try {
        fn();
    } catch (const ::cpm::test::detail::Stop&) {
        throw;  // a failed REQUIRE ends the test case
    } catch (const std::exception& e) {
        return std::string(e.what());
    } catch (const std::string& s) {
        return s;
    } catch (const char* s) {
        return std::string(s);
    } catch (...) {
        return std::string("(an exception that is not a std::exception)");
    }
    return std::nullopt;
}

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

#define TEST(name) CPM_TEST_CASE(#name)

#define CHECK(...) CPM_TEST_CHECK(__VA_ARGS__)

// Shows both sides when they differ.
#define CHECK_EQ(a, b) CPM_TEST_CHECK((a) == (b))

// The expression must throw, with a message that contains `substring`.
#define CHECK_THROWS(expr, substring)                                                                                  \
    do {                                                                                                               \
        const std::optional<std::string> check_thrown_ = ::check::thrown_message([&] { static_cast<void>(expr); });   \
        const std::string check_wanted_(substring);                                                                    \
        if (!check_thrown_) {                                                                                          \
            CPM_TEST_FAIL_CHECK("CHECK_THROWS({}) threw nothing", #expr);                                             \
        } else if (check_thrown_->find(check_wanted_) == std::string::npos) {                                          \
            CPM_TEST_FAIL_CHECK("CHECK_THROWS({}) threw \"{}\", which does not contain \"{}\"", #expr, *check_thrown_, \
                                check_wanted_);                                                                        \
        }                                                                                                              \
    } while (false)

// Shown with the failures that happen while it is in scope.
#define INFO(...) CPM_TEST_INFO(__VA_ARGS__)
