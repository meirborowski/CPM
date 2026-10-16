#include "build/runner.hpp"

#include <format>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"

namespace cpm::build {

fs::path find_ninja(const std::optional<std::string>& configured, const fs::path& home) {
    if (auto v = env::get("CPM_NINJA"); v && !v->empty()) {
        if (auto p = proc::find_program(*v)) return *p;
        throw Error(std::format("CPM_NINJA is set to '{}', which does not exist", *v));
    }
    if (configured) {
        if (auto p = proc::find_program(*configured)) return *p;
        throw Error(std::format("\"ninja\" in config.jsonc is '{}', which does not exist", *configured));
    }
    if (auto p = proc::find_program("ninja")) return *p;
#ifdef _WIN32
    fs::path bundled = home / "tools" / "ninja.exe";
#else
    fs::path bundled = home / "tools" / "ninja";
#endif
    std::error_code ec;
    if (fs::is_regular_file(bundled, ec)) return bundled;
    throw Error("ninja not found", "install Ninja 1.10 or newer (https://ninja-build.org) and put it on PATH, "
                                   "or set CPM_NINJA to its path");
}

namespace {

std::vector<std::string> base_args(const NinjaRun& run) {
    std::vector<std::string> args = {paths::to_utf8(run.ninja), "-f", run.build_file};
    if (run.jobs > 0) args.push_back(std::format("-j{}", run.jobs));
    return args;
}

std::vector<std::pair<std::string, std::string>> ninja_env() {
    std::vector<std::pair<std::string, std::string>> env = {{"NINJA_STATUS", "[%f/%t] "}};
    // Ninja decides on its own whether its output is a terminal; these make it follow CPM's choice.
    if (log::color()) env.emplace_back("CLICOLOR_FORCE", "1");
    else env.emplace_back("TERM", "dumb");
    return env;
}

}  // namespace

int run_ninja(const NinjaRun& run) {
    std::vector<std::string> args = base_args(run);
    if (run.verbose) args.push_back("-v");
    args.insert(args.end(), run.targets.begin(), run.targets.end());
    proc::Command cmd{.argv = args, .cwd = run.root, .env_set = ninja_env()};
    proc::RunOptions opt;
    opt.stdout_mode = proc::Stdout::ToStderr;
    opt.forward_interrupts = true;  // Ctrl-C stops Ninja, which reports it; CPM then cleans up
    return proc::run(cmd, opt);
}

std::string dry_run(const NinjaRun& run) {
    std::vector<std::string> args = base_args(run);
    args.push_back("-n");
    args.insert(args.end(), run.targets.begin(), run.targets.end());
    proc::Command cmd{.argv = args, .cwd = run.root, .env_set = {{"NINJA_STATUS", "[%f/%t] "}, {"TERM", "dumb"}}};
    proc::Captured r = proc::capture(cmd);
    if (r.exit_code != 0) throw Error(std::format("ninja -n failed:\n{}{}", r.out, r.err));
    return r.out;
}

void update_compile_commands(const NinjaRun& run) {
    std::vector<std::string> args = base_args(run);
    args.insert(args.end(), {"-t", "compdb", "cxx"});
    proc::Command cmd{.argv = args, .cwd = run.root};
    proc::Captured r = proc::capture(cmd);
    if (r.exit_code != 0) {
        log::warn(std::format("could not generate compile_commands.json: {}", r.err));
        return;
    }
    fsx::write_if_changed(run.root / "compile_commands.json", r.out);
}

}  // namespace cpm::build
