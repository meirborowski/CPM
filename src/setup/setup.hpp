#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// `cpm setup` (docs/spec/cli.md): makes the machine ready to build, installing a compiler and Ninja where they
// are missing. The pieces that decide what to do are separate from the ones that do it, so they can be tested.
namespace cpm::setup {

namespace fs = std::filesystem;

// A tool from toolchains.json (docs/spec/registry.md §8.2).
struct Tool {
    std::string name;     // for messages
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
    std::string program;  // the executable inside the archive ("cxx" or "exe"), '/'-separated
};

struct PlatformTools {
    std::optional<Tool> compiler;
    std::optional<Tool> ninja;
};

// "<arch>-<os>" for the machine this CPM was built for ("x86_64-windows", "aarch64-macos", ...).
std::string platform_key();

// The tools toolchains.json offers for `platform` (none if it lists nothing for it). Throws cpm::Error for an
// invalid document.
PlatformTools parse_toolchains(std::string_view text, const std::string& platform);

// ---- Linux packages

struct Needs {
    bool compiler = false;
    bool ninja = false;
    bool git = false;
    bool curl = false;
};

// The package managers cpm setup knows, in the order it looks for them.
const std::vector<std::string>& package_managers();

// The packages `manager` should install for what is missing. `compiler_package` is the GCC package for apt-get
// (see apt_compiler_package); the other managers' default GCC is recent enough.
std::vector<std::string> packages_for(const std::string& manager, const Needs& needs, const std::string& compiler_package);

// The command that installs `packages`, with sudo in front unless running as root.
std::vector<std::string> install_command(const std::string& manager, const std::vector<std::string>& packages, bool root);

// apt-get: the newest GCC package the distribution has, from g++-15 down to g++-13, else plain g++.
std::string apt_compiler_package(const std::function<bool(std::string_view)>& available);

// Whether `apt-cache policy <package>` output offers a version of exactly `package`. apt treats a name that no
// package has as a regular expression ("g++-15" matches clang-15), so other packages' entries must not count.
bool apt_policy_offers(std::string_view policy, std::string_view package);

// ---- configuration

// Sets a top-level key of $CPM_HOME/config.jsonc, creating the file if needed and keeping its comments.
void set_config(const fs::path& home, const std::string& key, const std::string& value);

struct Options {
    bool check = false;    // report only
    bool yes = false;      // do not ask
    bool managed = false;  // install CPM's own tools even when others work
};

// The whole command; returns the exit code (0 when the machine is ready).
int run(const Options& options, const fs::path& home);

}  // namespace cpm::setup
