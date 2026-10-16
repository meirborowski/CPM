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
};

struct Resolution {
    std::map<std::string, ResolvedPackage> packages;  // by name: one version per name (flat)
    std::vector<std::string> order;                   // dependency order: every package before its dependencies
};

// Flat resolution (docs/spec/dependencies.md §4.2): every package name gets exactly one version that
// satisfies every constraint on it. Locked versions (and git commits) are preferred when they still fit.
// Throws cpm::Error with the conflicting constraints when no solution exists.
Resolution resolve(const manifest::Manifest& root, Sources& sources, const lock::Lockfile* lock);

}  // namespace cpm::install
