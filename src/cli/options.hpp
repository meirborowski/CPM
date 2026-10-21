#pragma once

#include <string>

#include <functional>
#include <vector>

#include "build/pipeline.hpp"
#include "cli/args.hpp"
#include "manifest/config.hpp"

namespace cpm::cli {

// Shared option handling for commands.

// --profile / --release (default "debug").
std::string profile_option(const ParsedArgs& args);

// -j/--jobs, falling back to "jobs" in config.jsonc; 0 = Ninja's default.
int jobs_option(const ParsedArgs& args, const manifest::UserConfig& config);

// --toolchain or --cxx (build.md §1.1); using both is a usage error.
std::optional<toolchain::Choice> toolchain_option(const ParsedArgs& args);

// $CPM_HOME/config.jsonc, with its warnings printed.
manifest::UserConfig load_config();

// The dependency provider for builds: installs when cpm-lock.json or cpm_modules is out of date.
std::function<std::vector<build::Dependency>(const build::Project&)> dependency_provider(const manifest::UserConfig& config);

}  // namespace cpm::cli
