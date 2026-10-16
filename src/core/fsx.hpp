#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpm::fsx {

namespace fs = std::filesystem;

// Whole-file I/O in binary mode. read_file throws cpm::Error if the file cannot be read.
std::string read_file(const fs::path& p);
std::optional<std::string> try_read_file(const fs::path& p);

// Writes to a temporary file next to `p`, then renames it over `p`. Creates parent directories.
void write_file_atomic(const fs::path& p, std::string_view content);

// Writes only if the content differs from what is on disk. Returns true if it wrote.
bool write_if_changed(const fs::path& p, std::string_view content);

// Renames, replacing an existing target (atomic on the same volume).
void rename_replace(const fs::path& from, const fs::path& to);

// True for symlinks and, on Windows, for every reparse point (junctions included). Never follows links.
bool is_link(const fs::path& p);

// Deletes a file or directory tree WITHOUT descending into links or reparse points: links are removed
// as links, their targets are never touched (docs/spec/store.md §7). Missing paths are fine.
// Refuses to delete a filesystem root, the home directory, $CPM_HOME or $CPM_HOME/store.
void remove_tree(const fs::path& p);

void create_dirs(const fs::path& p);

// How a directory link was made (docs/spec/store.md §6).
enum class LinkKind { Symlink, Junction, Copy };
std::string_view to_string(LinkKind kind);

// Makes `link` (which must not exist) refer to the directory `target`: a symlink, else on Windows a
// junction (no privileges needed), else a recursive copy. `only` restricts the method. Returns the method
// used; throws cpm::Error if none works.
LinkKind link_directory(const fs::path& target, const fs::path& link, std::optional<LinkKind> only = std::nullopt);

struct DirEntry {
    std::string name;  // UTF-8
    bool is_dir = false;
    bool is_file = false;
    bool is_link = false;  // symlink or reparse point (never followed)
};

// The entries of a directory (without "." and ".."), using one native listing call per directory.
// A missing or unreadable directory yields an empty list.
std::vector<DirEntry> list_dir(const fs::path& dir);

}  // namespace cpm::fsx
