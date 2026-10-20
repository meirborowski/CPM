#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// What CPM keeps in $CPM_HOME, which projects use it, and cleaning up (docs/spec/store.md §8, cli.md `cpm cache`).
namespace cpm::install::cache {

namespace fs = std::filesystem;

// Records a project's root in $CPM_HOME/projects.json with today's date (store.md §8.1). Writes only when the
// project is new or was last recorded before today. Never throws: a project that cannot be recorded is only
// unknown to `cpm cache clean`.
void register_project(const fs::path& home, const fs::path& root);

// The registered projects: their root ('/'-separated) and when they last ran `cpm install` or `cpm build` (UTC date).
std::map<std::string, std::string> projects(const fs::path& home);

// How a project's root is recorded: absolute, normalized, '/'-separated, with an upper-case drive letter on Windows.
std::string project_key(const fs::path& root);

// Something in $CPM_HOME that cleaning may remove.
struct Item {
    enum class Kind { Package, Archive, Mirror, Temporary, Other };
    Kind kind;
    fs::path path;
    std::string label;  // how to name it: "fmt@12.2.0", "archive of fmt@12.2.0", a mirror's URL, a path
    std::uintmax_t bytes = 0;
};

struct Survey {
    std::size_t packages = 0, libraries = 0, archives = 0, mirrors = 0;
    std::uintmax_t store_bytes = 0, download_bytes = 0, git_bytes = 0;
    std::vector<std::string> projects;  // registered projects that still exist
    std::vector<std::string> gone;      // registered projects whose directory or cpm.jsonc is gone
    std::vector<Item> unused;           // what `cpm cache clean` removes (store.md §8.3)
    std::vector<std::string> unreadable;  // projects whose cpm-lock.json could not be read, with the reason
};

// Looks at the store, the caches and the registered projects' lockfiles. Changes nothing.
Survey survey(const fs::path& home);

struct CleanResult {
    std::vector<Item> removed;          // or, with dry_run, what would be removed
    std::vector<Item> busy;             // in use by another cpm process right now, so kept
    std::vector<std::string> forgotten;  // projects removed from projects.json
    std::uintmax_t bytes = 0;
};

// `cpm cache clean` (store.md §8.3): forgets projects that are gone, then removes what no remaining project uses
// and stale temporary directories. `all` empties the store, cache/, tmp/ and logs/ instead. `dry_run` only reports.
CleanResult clean(const fs::path& home, bool all, bool dry_run);

// "41.2 MB"
std::string format_bytes(std::uintmax_t bytes);

}  // namespace cpm::install::cache
