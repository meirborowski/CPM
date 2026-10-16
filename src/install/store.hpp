#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "json/value.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"

// The global store (docs/spec/store.md §2): $CPM_HOME/store/<entry>/ holds an extracted package, plus
// compiled artifacts under lib/<abi16>/.
namespace cpm::install {

namespace fs = std::filesystem;

// "<name>@<version>", or "<name>@<version>+g<commit12>" for git packages.
std::string entry_name(const lock::Package& package);

// The entry's directory. A registry package whose name@version is already in the store with a
// different integrity (another registry) gets "+s<first 8 hex of its integrity>" (store.md §2.1).
fs::path entry_dir(const fs::path& home, const lock::Package& package);

// Extracts a git package's commit into the store unless it is already there (under a lock, staged in
// $CPM_HOME/tmp and renamed into place). `overlay` is recorded with the entry. Returns the entry directory.
fs::path ensure_git_entry(const fs::path& home, const lock::Package& package, const std::optional<json::Value>& overlay,
                          bool offline);

// Downloads (or takes from the download cache) a registry package's archive, verifies its integrity
// and extracts it into the store unless it is already there. `strip` and `overlay` come from the index.
fs::path ensure_registry_entry(const fs::path& home, const lock::Package& package, int strip,
                               const std::optional<json::Value>& overlay, bool allow_http, bool offline);

// The effective manifest of a store entry: its cpm.jsonc with the recorded overlay applied.
manifest::Manifest entry_manifest(const fs::path& entry, const lock::Package& package);

// A unique scratch directory in $CPM_HOME/tmp (same volume as the store, so renames are atomic).
fs::path make_temp_dir(const fs::path& home, std::string_view prefix);

}  // namespace cpm::install
