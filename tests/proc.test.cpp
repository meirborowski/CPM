#include "core/proc.hpp"
#include "support/check.hpp"

using namespace cpm;

TEST(windows_quoting_follows_msvcrt_rules) {
    CHECK_EQ(cpm::proc::quote_windows("plain"), "plain");
    CHECK_EQ(cpm::proc::quote_windows(""), "\"\"");
    CHECK_EQ(cpm::proc::quote_windows("a b"), "\"a b\"");
    CHECK_EQ(cpm::proc::quote_windows("say \"hi\""), "\"say \\\"hi\\\"\"");
    CHECK_EQ(cpm::proc::quote_windows("C:\\dir with space\\"), "\"C:\\dir with space\\\\\"");
    CHECK_EQ(cpm::proc::quote_windows("a\\\\b c"), "\"a\\\\b c\"");  // backslashes not before a quote stay single
    CHECK_EQ(cpm::proc::quote_windows("C:\\no\\space"), "C:\\no\\space");
}

TEST(posix_quoting) {
    CHECK_EQ(cpm::proc::quote_posix("-Isrc"), "-Isrc");
    CHECK_EQ(cpm::proc::quote_posix("a b"), "'a b'");
    CHECK_EQ(cpm::proc::quote_posix("it's"), "'it'\\''s'");
    CHECK_EQ(cpm::proc::quote_posix("$ORIGIN"), "'$ORIGIN'");
    CHECK_EQ(cpm::proc::quote_posix(""), "''");
}

TEST(finds_programs_on_path) {
    CHECK(!cpm::proc::find_program("definitely-not-a-program-cpm-test").has_value());
#ifdef _WIN32
    CHECK(cpm::proc::find_program("cmd").has_value());
#else
    CHECK(cpm::proc::find_program("sh").has_value());
#endif
}

#ifdef _WIN32
static cpm::proc::Command shell(std::string script) { return {.argv = {"cmd", "/d", "/c", std::move(script)}}; }
#else
static cpm::proc::Command shell(std::string script) { return {.argv = {"sh", "-c", std::move(script)}}; }
#endif

TEST(captures_output_and_exit_code) {
    auto r = cpm::proc::capture(shell("echo out&& echo err 1>&2&& exit 3"));
    CHECK_EQ(r.exit_code, 3);
    CHECK(r.out.find("out") != std::string::npos);
    CHECK(r.err.find("err") != std::string::npos);
    CHECK(!r.timed_out);
}

TEST(passes_environment_and_cwd) {
    check::TempDir dir;
    cpm::proc::Command cmd = shell(
#ifdef _WIN32
        "echo %CPM_TEST_VAR%&& cd"
#else
        "echo $CPM_TEST_VAR && pwd"
#endif
    );
    cmd.env_set = {{"CPM_TEST_VAR", "hello world"}};
    cmd.cwd = dir.path();
    auto r = cpm::proc::capture(cmd);
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("hello world") != std::string::npos);
    CHECK(r.out.find(dir.path().filename().string()) != std::string::npos);
}

TEST(kills_on_timeout) {
    // The shell starts a grandchild that holds the output pipes, so killing only the shell would leave
    // capture() waiting for the full 30 seconds.
    auto start = std::chrono::steady_clock::now();
#ifdef _WIN32
    auto r = cpm::proc::capture(shell("ping -n 30 127.0.0.1 >nul& echo done"), std::chrono::milliseconds(300));
#else
    auto r = cpm::proc::capture(shell("sleep 30; echo done"), std::chrono::milliseconds(300));
#endif
    CHECK(r.timed_out);
    CHECK_EQ(r.exit_code, -1);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST(feeds_input_to_standard_input) {
    cpm::proc::Command cmd =
#ifdef _WIN32
        shell("findstr x");
#else
        shell("grep x");
#endif
    cmd.input = std::string("one x\ntwo\nthree x\n") + std::string(200000, 'y') + "\n";  // larger than a pipe buffer
    auto r = cpm::proc::capture(cmd);
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("one x") != std::string::npos);
    CHECK(r.out.find("three x") != std::string::npos);
    CHECK(r.out.find("two") == std::string::npos);
}

TEST(missing_program_is_an_error) {
    CHECK_THROWS(cpm::proc::capture({.argv = {"definitely-not-a-program-cpm-test"}}), "program not found");
}
