#pragma once

#include <string>
#include <vector>

#include "build/pipeline.hpp"
#include "build/plan.hpp"
#include "lock/lockfile.hpp"
#include "manifest/config.hpp"

namespace cpm::install {

struct Options {
    std::filesystem::path cwd{};        // where relative paths on the command line are resolved
    std::vector<std::string> add{};     // cpm install <spec>...
    bool dev = false;                   // -D: add to devDependencies
    std::vector<std::string> remove{};  // cpm remove <name>...
    bool frozen = false;              // --frozen-lockfile
    bool offline = false;             // --offline
    bool prefer_offline = false;      // --prefer-offline: cached registry indexes of any age
    bool no_build = false;            // --no-build: skip building dependency binaries
    bool exact = false;               // -E: save the exact version for bare registry names
    std::string registry{};           // --registry: for the registry specs being added
    // cpm update: resolve without the lockfile's preferences for these packages (dependencies.md §4.5).
    std::vector<std::string> update{};
    bool update_all = false;          // cpm update without names: for every package
};

// `cpm install` / `cpm remove` / `cpm update` (docs/spec/cli.md): applies the manifest changes, resolves,
// fetches, builds and links, and only then writes cpm.jsonc and cpm-lock.json. Returns the dependencies for a
// build.
std::vector<build::Dependency> install(build::Project& project, const Options& options, const manifest::UserConfig& config);

// For `cpm build`, `cpm test` and `cpm run`: the dependencies, installing first when cpm-lock.json is
// missing or stale, and relinking when cpm_modules does not match it.
std::vector<build::Dependency> ensure(const build::Project& project, const manifest::UserConfig& config);

// The current manifest of a path package of the lockfile, with the overlay the project's manifest gives it.
manifest::Manifest path_package_manifest(const std::filesystem::path& root_dir, const manifest::Manifest& root,
                                         const lock::Package& package);

}  // namespace cpm::install
