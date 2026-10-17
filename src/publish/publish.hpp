#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "manifest/config.hpp"
#include "manifest/manifest.hpp"

// `cpm publish` (docs/spec/registry.md §6): packs a library into a deterministic source archive and adds a
// version to a static registry's index.
namespace cpm::publish {

namespace fs = std::filesystem;

struct Options {
    // Every member has a default so callers can use designated initializers for the ones they need.
    std::string registry{};          // name or URL; empty = the default registry
    std::optional<fs::path> out{};   // write the archive and the index document here instead of into the registry
    bool dry_run = false;
};

// What publishing produces, before anything is written.
struct Package {
    std::string name;
    std::string version;
    manifest::TargetKind type = manifest::TargetKind::StaticLib;  // never Executable
    std::vector<std::string> files;  // package-relative paths, sorted
    std::string archive;             // .tar.gz bytes
    std::string integrity;           // sha256:<hex>
    std::string archive_path;        // registry-relative: packages/<name>/<name>-<version>.tar.gz
};

// The files published from a package: cpm.jsonc, license and readme files, plus `files` from the manifest,
// or by default the include directory, src/ and the source globs (registry.md §6). Never cpm_modules/,
// .git/, build/ or cpm-lock.json.
std::vector<std::string> select_files(const manifest::Manifest& m);

// Checks that the package can be published and packs it. Throws cpm::Error otherwise.
Package pack(const manifest::Manifest& m);

// The index document (index/<name>.json) with `pkg` added to `existing` (the current document, if any).
// Throws if the version is already published.
std::string add_version(const std::optional<std::string>& existing, const manifest::Manifest& m, const Package& pkg,
                        const std::string& registry_base);

// The whole command: pack, then write into a file:// registry (under its lock), or into `out`.
void publish(const fs::path& root, const Options& options, const manifest::UserConfig& config, const fs::path& home);

}  // namespace cpm::publish
