#include "core/paths.hpp"
#include "core/proc.hpp"
#include "run/run.hpp"
#include "support/check.hpp"
#include "toolchain/toolchain.hpp"

using namespace cpm;
namespace fs = std::filesystem;

namespace {

run::Environment test_env(const fs::path& root) {
    run::Environment env;
    env.root = root;
    env.profile = "release";
    env.package_name = "demo";
    env.package_version = "1.2.3";
    env.bin_dir = root / "build" / "release";
    env.cpm_exe = root / "tools" / "cpm";
    return env;
}

std::string value_of(const std::vector<std::pair<std::string, std::string>>& vars, const std::string& name) {
    for (const auto& [n, v] : vars) {
        if (n == name) return v;
    }
    return "<unset>";
}

}  // namespace

TEST(cmd_quoting_escapes_metacharacters) {
    CHECK_EQ(cpm::proc::quote_cmd("plain"), "plain");
    CHECK_EQ(cpm::proc::quote_cmd("a&b"), "a^&b");
    CHECK_EQ(cpm::proc::quote_cmd("a b"), "^\"a b^\"");
    CHECK_EQ(cpm::proc::quote_cmd("x|y>z<w"), "x^|y^>z^<w");
    CHECK_EQ(cpm::proc::quote_cmd("(p)!"), "^(p^)^!");
    CHECK_EQ(cpm::proc::quote_cmd("q\"t"), "^\"q\\^\"t^\"");
}

TEST(environment_variables) {
    check::TempDir dir;
    auto vars = run::variables(test_env(dir.path()));
    CHECK_EQ(value_of(vars, "CPM_PROFILE"), "release");
    CHECK_EQ(value_of(vars, "CPM_PACKAGE_NAME"), "demo");
    CHECK_EQ(value_of(vars, "CPM_PACKAGE_VERSION"), "1.2.3");
    std::string path = value_of(vars, "PATH");
    std::string bin = value_of(vars, "CPM_BIN_DIR");
    CHECK(path.starts_with(bin));  // the profile's bin directory comes first
    CHECK(bin.ends_with("release"));
#ifdef _WIN32
    CHECK(bin.find('/') == std::string::npos);  // native separators for other programs
#endif
}

TEST(the_compilers_runtime_libraries_are_found_on_windows) {
    // MinGW programs load libstdc++ and friends from the compiler's bin directory; MSVC's runtime directories are
    // those vcvarsall.bat puts on PATH (build.md §1.5).
    toolchain::Toolchain mingw;
    mingw.os = Os::Windows;
    mingw.cxx = "C:/w/mingw64/bin/g++.exe";
    CHECK(toolchain::runtime_dirs(mingw) == std::vector<fs::path>{"C:/w/mingw64/bin"});
    toolchain::Toolchain msvc = mingw;
    msvc.family = toolchain::Family::Msvc;
    msvc.env = {{"PATH", "C:/vs/bin/Hostx64/x64;C:/sdk/bin"}};
    CHECK(toolchain::runtime_dirs(msvc) == (std::vector<fs::path>{"C:/vs/bin/Hostx64/x64", "C:/sdk/bin"}));
    toolchain::Toolchain linux_gcc;
    linux_gcc.os = Os::Linux;
    linux_gcc.cxx = "/usr/bin/g++";
    CHECK(toolchain::runtime_dirs(linux_gcc).empty());

    check::TempDir dir;
    auto env = test_env(dir.path());
    env.runtime_dirs = {dir.path() / "mingw64" / "bin"};
    std::string path = value_of(run::variables(env), "PATH");
    std::string runtime = paths::to_native_utf8(env.runtime_dirs[0]);
    CHECK(path.find(runtime) != std::string::npos);
    CHECK(path.find(runtime) > path.find(value_of(run::variables(env), "CPM_BIN_DIR")));  // after the project's own programs
}

TEST(scripts_receive_arguments_exactly) {
    check::TempDir dir;
    std::vector<std::string> args = {"a b", "x&y", "p|q", "(z)", "q\"t", ""};
#ifdef _WIN32
    // cmd's echo prints its arguments as written (quoted where needed), not as a program would parse them.
    auto r = cpm::proc::capture(run::script_command("echo", args, test_env(dir.path())));
    CHECK_EQ(r.exit_code, 0);
    CHECK_EQ(r.out, "\"a b\" x&y p|q (z) \"q\\\"t\" \"\"\r\n");
#else
    auto r = cpm::proc::capture(run::script_command("printf '[%s]'", args, test_env(dir.path())));
    CHECK_EQ(r.exit_code, 0);
    CHECK_EQ(r.out, "[a b][x&y][p|q][(z)][q\"t][]");
#endif
}

TEST(scripts_run_in_the_project_root_with_the_environment) {
    check::TempDir dir;
#ifdef _WIN32
    auto r = cpm::proc::capture(run::script_command("echo %CPM_PACKAGE_NAME% %CPM_PROFILE%&& cd", {}, test_env(dir.path())));
#else
    auto r = cpm::proc::capture(run::script_command("echo $CPM_PACKAGE_NAME $CPM_PROFILE && pwd", {}, test_env(dir.path())));
#endif
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("demo release") != std::string::npos);
    CHECK(r.out.find(dir.path().filename().string()) != std::string::npos);
}

TEST(script_exit_codes_propagate) {
    check::TempDir dir;
    CHECK_EQ(cpm::proc::capture(run::script_command("exit 7", {}, test_env(dir.path()))).exit_code, 7);
    CHECK_EQ(cpm::proc::run(run::script_command("exit 3", {}, test_env(dir.path())), {.null_stdin = true}), 3);
}

TEST(program_command) {
    check::TempDir dir;
    auto cmd = run::program_command(dir.path() / "app", {"--x", "y z"}, dir.path(), test_env(dir.path()));
    CHECK_EQ(cmd.argv.size(), 3u);
    CHECK_EQ(cmd.argv[1], "--x");
    CHECK_EQ(cmd.argv[2], "y z");
    CHECK(cmd.cwd == dir.path());
    CHECK(!cmd.verbatim_args);
}
