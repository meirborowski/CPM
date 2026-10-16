#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "build/pipeline.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"

namespace cpm::install {

namespace fs = std::filesystem;

// What a store library needs from its own dependencies to compile.
struct DependencyInputs {
    std::vector<fs::path> include_dirs;       // absolute
    std::vector<std::string> public_defines;
};

// Builds a static library from a store entry for the project's ABI into <entry>/lib/<abi16>/, unless it
// is already there and intact (docs/spec/build.md §5, store.md §2.3). Runs in a sandbox under $CPM_HOME/tmp; the output goes to
// $CPM_HOME/logs and is shown when the build fails. Returns the lib/<abi16> directory.
fs::path ensure_library(const build::Project& project, const lock::Package& package, const fs::path& entry,
                        const manifest::Manifest& manifest, const DependencyInputs& inputs, const fs::path& ninja);

// Installs a registry's prebuilt binary archive for the project's ABI into <entry>/lib/<abi16>/
// (registry.md §4.2). Returns false, after a warning, when the archive's meta.json was built for different
// ABI inputs; the caller then builds from source.
bool install_prebuilt(const build::Project& project, const lock::Package& package, const fs::path& entry,
                      const std::string& url, const std::string& integrity, bool allow_http, bool offline);

}  // namespace cpm::install
