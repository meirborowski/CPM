#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cpm::glob {

// Expands {a,b} alternatives (nesting allowed): "src/*.{cpp,cc}" -> {"src/*.cpp", "src/*.cc"}.
std::vector<std::string> expand_braces(std::string_view pattern);

// Matches a '/'-separated relative path against a pattern with '*' (within one segment), '?',
// '**' (any number of whole segments, including zero) and {a,b}. Case-sensitive.
bool match(std::string_view pattern, std::string_view path);

// Files under `root` selected by `patterns`, applied in order: a pattern adds its matches, a pattern
// starting with '!' removes its matches. Returns root-relative generic UTF-8 paths, sorted byte-wise.
// Links and reparse points are never followed; directories named cpm_modules or .git are skipped.
std::vector<std::string> find(const std::filesystem::path& root, const std::vector<std::string>& patterns);

}  // namespace cpm::glob
