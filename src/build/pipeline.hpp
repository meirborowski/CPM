#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "build/plan.hpp"
#include "manifest/config.hpp"
#include "toolchain/abi.hpp"

namespace cpm::build {

// The root package with its profile and toolchain: what build, test, run and install share.
struct Project {
    std::filesystem::path root;
    manifest::Manifest manifest;
    manifest::Profile profile;
    toolchain::Toolchain toolchain;
    std::string std_flag;
    std::shared_ptr<const toolchain::Driver> driver;
    toolchain::AbiFingerprint abi;  // of the root's settings (abi.md)
    std::filesystem::path home;     // $CPM_HOME
};

// Finds the project root from `cwd`, loads the manifest and the profile, and probes the toolchain.
Project load_project(const std::filesystem::path& cwd, const std::string& profile,
                     const std::optional<std::string>& cxx, const manifest::UserConfig& config);

// Reloads the manifest (after `cpm install` edited it) and everything derived from it.
void reload_manifest(Project& project);

struct BuildRequest {
    std::filesystem::path cwd;
    std::string profile = "debug";
    std::optional<std::string> cxx;
    int jobs = 0;
    enum class Targets { Main, MainAndTests, Tests } targets = Targets::Main;
    std::vector<std::string> test_filters;  // for Targets::Tests: substrings of test names
    bool clean = false;
    bool require_executable = false;  // fail before building unless the package is an executable (cpm run)
    // Supplies the dependencies in dependency order, installing them if needed (install::ensure). Required
    // when the package has dependencies; keeping it a callback keeps build/ independent of install/.
    std::function<std::vector<Dependency>(const Project&)> dependencies;
};

struct BuildResult {
    int exit_code = 0;
    BuildPlan plan;
    std::vector<std::size_t> selected_tests;  // indices into plan.tests
    double seconds = 0;
};

// The whole `cpm build` pipeline (docs/spec/build.md, overview): manifest, profile, toolchain, dependencies,
// plan, build.ninja, Ninja. Prints progress. Throws cpm::Error for problems before Ninja runs.
BuildResult run_build(const BuildRequest& request, const manifest::UserConfig& config);

}  // namespace cpm::build
