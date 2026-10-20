#pragma once

#include <optional>
#include <string>
#include <vector>

#include "install/registry.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"
#include "semver/range.hpp"
#include "semver/version.hpp"

// `cpm outdated` (docs/spec/cli.md): how the direct registry dependencies compare with their registries.
namespace cpm::install {

struct OutdatedEntry {
    std::string name;
    bool dev = false;
    std::optional<semver::Version> current{};  // from cpm-lock.json; nullopt when it has none
    std::optional<semver::Version> wanted{};   // the newest version the manifest's range allows
    std::optional<semver::Version> latest{};   // the newest version

    // Current, Wanted and Latest are not all the same version.
    bool outdated() const;
};

struct OutdatedReport {
    std::vector<OutdatedEntry> entries;  // every registry dependency: dependencies, then devDependencies
    std::vector<const manifest::DependencySpec*> skipped;  // path and git dependencies
};

// The version a resolution would try first among those `range` allows (all versions when it is null), skipping
// yanked ones: the highest release, or the highest prerelease when no release qualifies (dependencies.md §4.2).
std::optional<semver::Version> newest(const RegistryPackage& package, const semver::Range* range);

// Compares the manifest's direct dependencies with cpm-lock.json (`lock` may be null) and their registries' indexes.
// Throws cpm::Error when a registry cannot be read or has no such package.
OutdatedReport outdated(const manifest::Manifest& manifest, const lock::Lockfile* lock, Registries& registries);

// The project's overrides that are no longer needed (dependencies.md §4.6), each as a note: every requirement it
// replaced, at the dependent's own range, allows the version cpm-lock.json has, or it replaced nothing.
std::vector<std::string> unneeded_overrides(const manifest::Manifest& manifest, const lock::Lockfile& lock);

}  // namespace cpm::install
