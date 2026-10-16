#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/fsx.hpp"

// cpm_modules/ (docs/spec/store.md §5): cpm_modules/<name>/{include,lib} are links into the store or to
// path packages, and .cpm-modules.json records what they point at.
namespace cpm::install {

namespace fs = std::filesystem;

struct LinkTarget {
    std::string name;
    std::string version;
    std::optional<fs::path> include;  // absolute directory, if the package has public headers
    std::optional<fs::path> lib;      // absolute store lib/<abi16> directory, for store libraries
};

// True if .cpm-modules.json says cpm_modules matches this lockfile and ABI.
bool modules_current(const fs::path& root, const std::string& lock_hash, const std::string& abi);

// Makes cpm_modules match `targets`: (re)creates links that differ, removes packages that are gone, and
// writes .cpm-modules.json. `mode` forces a link method (config "linkMode").
void link_modules(const fs::path& root, const std::vector<LinkTarget>& targets, const std::string& lock_hash,
                  const std::string& abi, std::optional<fsx::LinkKind> mode);

}  // namespace cpm::install
