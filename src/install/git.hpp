#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Git operations for git dependencies, on bare mirrors in $CPM_HOME/cache/git (docs/spec/store.md §3).
namespace cpm::install::git {

namespace fs = std::filesystem;

// $CPM_HOME/cache/git/<sha256(url)[:16]>.git
fs::path mirror_dir(const fs::path& home, std::string_view url);

// Clones the mirror if it does not exist yet (under a lock). Returns its directory and whether it was just
// cloned (a fresh clone needs no fetch). Fails when offline and the mirror is missing.
std::pair<fs::path, bool> ensure_mirror(const fs::path& home, std::string_view url, bool offline);

// Updates the mirror from its remote.
void fetch(const fs::path& mirror, std::string_view url);

// The commit a ref (tag, branch, commit, or empty for the default branch) points to, if known locally.
std::optional<std::string> resolve(const fs::path& mirror, std::string_view ref);

// The contents of a file at a commit, or nullopt if it does not exist there.
std::optional<std::string> show_file(const fs::path& mirror, std::string_view commit, std::string_view path);

// Writes the tree of a commit as a tar file.
void archive(const fs::path& mirror, std::string_view commit, const fs::path& tar_file);

// "v1.2.3" or "1.2.3" -> "1.2.3" (dependencies.md §1.3); nullopt for other refs.
std::optional<std::string> version_from_tag(std::string_view ref);

}  // namespace cpm::install::git
