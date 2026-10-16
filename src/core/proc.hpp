#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpm::proc {

namespace fs = std::filesystem;

struct Command {
    // Every member has a default so callers can use designated initializers for the ones they need.
    std::vector<std::string> argv{};  // argv[0]: a path, or a name looked up on PATH
    fs::path cwd{};                   // empty: inherit
    std::vector<std::pair<std::string, std::string>> env_set{};  // added or overridden
    std::vector<std::string> env_unset{};
    // Windows only: append argv[1..] to the command line exactly as given, without quoting. Needed for
    // `cmd.exe /d /s /c "<command>"`, whose argument is not parsed by the C runtime rules.
    bool verbatim_args = false;
};

struct Captured {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};

enum class Stdout { Inherit, ToStderr };

struct RunOptions {
    Stdout stdout_mode = Stdout::Inherit;
    bool null_stdin = false;
    std::optional<std::chrono::milliseconds> timeout{};
    // While the child runs, CPM ignores Ctrl-C (SIGINT/SIGQUIT) and lets the child decide what to do;
    // a child killed by Ctrl-C yields 130.
    bool forward_interrupts = false;
};

// Runs a command and captures stdout and stderr. Throws cpm::Error if the program cannot be started.
// On timeout the process is killed and `timed_out` is set.
Captured capture(const Command& cmd, std::optional<std::chrono::milliseconds> timeout = std::nullopt,
                 bool null_stdin = true);

// Runs a command with inherited standard streams (stdout optionally redirected to our stderr).
// Returns the exit code (128 + signal for signals on POSIX, -1 on timeout).
int run(const Command& cmd, const RunOptions& options = {});

// Looks a program up on PATH (trying .exe and .com on Windows). Names containing a separator are checked as paths.
std::optional<fs::path> find_program(std::string_view name);

// Quoting for a command line: MS C runtime rules on Windows, POSIX shell rules elsewhere.
std::string quote_windows(std::string_view arg);
std::string quote_posix(std::string_view arg);
std::string quote(std::string_view arg);  // the host's rules
// Quoting for an argument appended to a cmd.exe command line: C runtime quoting, then cmd's
// metacharacters ( ) ! ^ " < > & | escaped with '^'. ('%' cannot be escaped on a cmd command line.)
std::string quote_cmd(std::string_view arg);
std::string command_line(const std::vector<std::string>& argv);  // for display and Ninja commands

}  // namespace cpm::proc
