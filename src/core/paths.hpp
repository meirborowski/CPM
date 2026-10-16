#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cpm::paths {

namespace fs = std::filesystem;

// UTF-8 text of a path with '/' separators (generic form).
std::string to_utf8(const fs::path& p);

// UTF-8 text of a normalized path with the OS's own separators, for values other programs read
// (environment variables such as PATH).
std::string to_native_utf8(const fs::path& p);

// Path from UTF-8 text. Never use fs::path(std::string) directly: on Windows it assumes the ANSI code page.
fs::path from_utf8(std::string_view text);

// $CPM_HOME, or ~/.cpm (%USERPROFILE%\.cpm on Windows).
fs::path cpm_home();

// The user's home directory.
fs::path home_dir();

// Absolute path of the running executable.
fs::path current_exe();

// The nearest directory at or above `start` that contains cpm.jsonc.
std::optional<fs::path> find_project_root(const fs::path& start);

// `p` relative to `base` if it lies inside `base`, otherwise the absolute path; always generic UTF-8.
std::string display(const fs::path& p, const fs::path& base);

// `p` relative to `base` when both are on the same root (drive); otherwise absolute. Generic UTF-8.
std::string relative_or_absolute(const fs::path& p, const fs::path& base);

// True if both paths name the same existing file.
bool same_file(const fs::path& a, const fs::path& b);

}  // namespace cpm::paths
