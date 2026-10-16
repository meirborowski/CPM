#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "manifest/manifest.hpp"
#include "semver/version.hpp"

// cpm-lock.json (docs/spec/lockfile.md).
namespace cpm::lock {

namespace fs = std::filesystem;

inline constexpr int kLockfileVersion = 1;

struct RegistryResolution {
    std::string registry, url, integrity;
};
struct GitResolution {
    std::string url, ref, commit;  // ref may be empty (default branch)
};
struct PathResolution {
    std::string path;  // relative to the project root, '/'-separated
};
using Resolution = std::variant<RegistryResolution, GitResolution, PathResolution>;

std::string_view kind_of(const Resolution& r);  // "registry" | "git" | "path"

struct Package {
    std::string name;
    semver::Version version;
    Resolution resolution;
    manifest::TargetKind type = manifest::TargetKind::StaticLib;
    std::string std = "c++20";
    std::map<std::string, semver::Version> dependencies;  // name -> exact version
    bool dev = false;

    std::string key() const;  // "name@version"
};

struct RootDependency {
    std::string specifier;
    semver::Version version;
};

struct Lockfile {
    std::map<std::string, RootDependency> dependencies;      // importers["."].dependencies
    std::map<std::string, RootDependency> dev_dependencies;  // importers["."].devDependencies
    std::map<std::string, Package> packages;                 // by "name@version"

    // The (single) locked package with this name.
    const Package* find(std::string_view name) const;
};

// Parses a lockfile. Throws DiagnosticsError for malformed content and cpm::Error for a newer format.
Lockfile parse(std::string_view text, const std::string& display);

// The canonical bytes (lockfile.md §3).
std::string serialize(const Lockfile& lock);

// <root>/cpm-lock.json, or nullopt if there is none.
std::optional<Lockfile> load(const fs::path& root, const fs::path& display_base);

// Writes <root>/cpm-lock.json if its bytes change. Returns true if it wrote.
bool save(const fs::path& root, const Lockfile& lock);

// Why the lockfile does not match the manifest (lockfile.md §4), or nullopt if it is fresh.
// `read_path_package` loads the current manifest of a path package. Throws cpm::Error if the lockfile
// is internally inconsistent (corrupt).
std::optional<std::string> staleness(const Lockfile& lock, const manifest::Manifest& root,
                                     const std::function<manifest::Manifest(const Package&)>& read_path_package);

}  // namespace cpm::lock
