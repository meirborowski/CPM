#include "core/proc.hpp"
#include "support/check.hpp"

using namespace cpm;

TEST(windows_quoting_follows_msvcrt_rules) {
    CHECK_EQ(proc::quote_windows("plain"), "plain");
    CHECK_EQ(proc::quote_windows(""), "\"\"");
    CHECK_EQ(proc::quote_windows("a b"), "\"a b\"");
    CHECK_EQ(proc::quote_windows("say \"hi\""), "\"say \\\"hi\\\"\"");
    CHECK_EQ(proc::quote_windows("C:\\dir with space\\"), "\"C:\\dir with space\\\\\"");
    CHECK_EQ(proc::quote_windows("a\\\\b c"), "\"a\\\\b c\"");  // backslashes not before a quote stay single
    CHECK_EQ(proc::quote_windows("C:\\no\\space"), "C:\\no\\space");
}

TEST(posix_quoting) {
    CHECK_EQ(proc::quote_posix("-Isrc"), "-Isrc");
    CHECK_EQ(proc::quote_posix("a b"), "'a b'");
    CHECK_EQ(proc::quote_posix("it's"), "'it'\\''s'");
    CHECK_EQ(proc::quote_posix("$ORIGIN"), "'$ORIGIN'");
    CHECK_EQ(proc::quote_posix(""), "''");
}

TEST(finds_programs_on_path) {
    CHECK(!proc::find_program("definitely-not-a-program-cpm-test").has_value());
#ifdef _WIN32
    CHECK(proc::find_program("cmd").has_value());
#else
    CHECK(proc::find_program("sh").has_value());
#endif
}

#ifdef _WIN32
static proc::Command shell(std::string script) { return {.argv = {"cmd", "/d", "/c", std::move(script)}}; }
#else
static proc::Command shell(std::string script) { return {.argv = {"sh", "-c", std::move(script)}}; }
#endif

TEST(captures_output_and_exit_code) {
    auto r = proc::capture(shell("echo out&& echo err 1>&2&& exit 3"));
    CHECK_EQ(r.exit_code, 3);
    CHECK(r.out.find("out") != std::string::npos);
    CHECK(r.err.find("err") != std::string::npos);
    CHECK(!r.timed_out);
}

TEST(passes_environment_and_cwd) {
    check::TempDir dir;
    proc::Command cmd = shell(
#ifdef _WIN32
        "echo %CPM_TEST_VAR%&& cd"
#else
        "echo $CPM_TEST_VAR && pwd"
#endif
    );
    cmd.env_set = {{"CPM_TEST_VAR", "hello world"}};
    cmd.cwd = dir.path();
    auto r = proc::capture(cmd);
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("hello world") != std::string::npos);
    CHECK(r.out.find(dir.path().filename().string()) != std::string::npos);
}

TEST(kills_on_timeout) {
#ifdef _WIN32
    auto r = proc::capture(shell("ping -n 30 127.0.0.1 >nul"), std::chrono::milliseconds(300));
#else
    auto r = proc::capture(shell("sleep 30"), std::chrono::milliseconds(300));
#endif
    CHECK(r.timed_out);
    CHECK_EQ(r.exit_code, -1);
}

TEST(missing_program_is_an_error) {
    CHECK_THROWS(proc::capture({.argv = {"definitely-not-a-program-cpm-test"}}), "program not found");
}
