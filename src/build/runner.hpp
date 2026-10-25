#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cpm::build {

namespace fs = std::filesystem;

// Ninja discovery (build.md §6): CPM_NINJA, the "ninja" config key, PATH, then $CPM_HOME/tools.
fs::path find_ninja(const std::optional<std::string>& configured, const fs::path& home);

struct NinjaRun {
    fs::path ninja;
    fs::path root;                    // working directory
    std::string build_file;           // relative to root
    std::vector<std::string> targets;
    int jobs = 0;                     // 0 = Ninja's default
    bool verbose = false;
};

// Runs Ninja with its output on our stderr. Returns Ninja's exit code.
int run_ninja(const NinjaRun& run);

// `ninja -n`: the commands Ninja would run, as printed (one status line per edge).
std::string dry_run(const NinjaRun& run);

// Writes <root>/compile_commands.json from `ninja -t compdb cxx cc` if it changed.
void update_compile_commands(const NinjaRun& run);

}  // namespace cpm::build
