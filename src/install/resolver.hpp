#pragma once

#include <map>
#include <string>
#include <vector>

#include "install/sources.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"

namespace cpm::install {

struct ResolvedPackage {
    PackageMeta meta;
    std::vector<std::string> dependents;  // "root" or "name@version"
    bool dev = false;                     // reachable only through the root's devDependencies
    std::map<std::string, std::string> overridden;  // dependency -> this package's own range, which an override replaced
};

struct Resolution {
    std::map<std::string, ResolvedPackage> packages;  // by name: one version per name (flat)
    std::vector<std::string> order;                   // dependency order: every package before its dependencies
    std::vector<std::string> unused_overrides;        // keys of the project's overrides that replaced no requirement
};

// Flat resolution with PubGrub (docs/spec/dependencies.md §4.2): every package name gets exactly one version that
// satisfies every constraint on it, after the project's overrides (§4.6). Locked versions (and git commits) are
// preferred when they still fit.
// Throws cpm::Error explaining why when no solution exists (§4.3).
Resolution resolve(const manifest::Manifest& root, Sources& sources, const lock::Lockfile* lock);

}  // namespace cpm::install
