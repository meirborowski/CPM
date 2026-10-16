#include "core/glob.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

namespace cpm::glob {

namespace fs = std::filesystem;

std::vector<std::string> expand_braces(std::string_view pattern) {
    // Find the first top-level '{' with a matching '}'.
    std::size_t open = std::string_view::npos;
    int depth = 0;
    std::size_t close = std::string_view::npos;
    std::vector<std::size_t> commas;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '{') {
            if (depth == 0) {
                open = i;
                commas.clear();
            }
            ++depth;
        } else if (c == '}' && depth > 0) {
            --depth;
            if (depth == 0) {
                close = i;
                break;
            }
        } else if (c == ',' && depth == 1) {
            commas.push_back(i);
        }
    }
    if (open == std::string_view::npos || close == std::string_view::npos) return {std::string(pattern)};

    std::string_view prefix = pattern.substr(0, open);
    std::string_view suffix = pattern.substr(close + 1);
    std::vector<std::string_view> alternatives;
    std::size_t start = open + 1;
    for (std::size_t comma : commas) {
        alternatives.push_back(pattern.substr(start, comma - start));
        start = comma + 1;
    }
    alternatives.push_back(pattern.substr(start, close - start));

    std::vector<std::string> out;
    for (auto alt : alternatives) {
        std::string combined = std::string(prefix) + std::string(alt) + std::string(suffix);
        for (auto& expanded : expand_braces(combined)) out.push_back(std::move(expanded));
    }
    return out;
}

namespace {

bool match_segment(std::string_view pat, std::string_view name) {
    std::size_t p = 0, n = 0;
    std::size_t star = std::string_view::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

bool match_parts(const std::vector<std::string>& pat, std::size_t pi, const std::vector<std::string>& path,
                 std::size_t si) {
    if (pi == pat.size()) return si == path.size();
    if (pat[pi] == "**") {
        for (std::size_t k = si; k <= path.size(); ++k) {
            if (match_parts(pat, pi + 1, path, k)) return true;
        }
        return false;
    }
    if (si == path.size()) return false;
    if (!match_segment(pat[pi], path[si])) return false;
    return match_parts(pat, pi + 1, path, si + 1);
}

bool has_magic(std::string_view segment) { return segment.find_first_of("*?") != std::string_view::npos; }

// Collects root-relative paths of the regular files below `dir`. Links are never followed.
void walk(const fs::path& dir, const std::string& rel, std::vector<std::string>& files) {
    for (const auto& e : fsx::list_dir(dir)) {
        if (e.is_link) continue;
        std::string child = rel.empty() ? e.name : rel + '/' + e.name;
        if (e.is_dir) {
            if (e.name == "cpm_modules" || e.name == ".git") continue;
            walk(dir / paths::from_utf8(e.name), child, files);
        } else if (e.is_file) {
            files.push_back(std::move(child));
        }
    }
}

}  // namespace

bool match(std::string_view pattern, std::string_view path) {
    std::vector<std::string> parts = str::split(path, '/');
    for (const auto& alt : expand_braces(pattern)) {
        if (match_parts(str::split(alt, '/'), 0, parts, 0)) return true;
    }
    return false;
}

std::vector<std::string> find(const fs::path& root, const std::vector<std::string>& patterns) {
    // Each directory is listed at most once, however many patterns start below it.
    std::map<std::string, std::vector<std::string>> walked;  // base (relative) -> files (root-relative)
    auto files_below = [&](const std::string& base) -> const std::vector<std::string>& {
        auto it = walked.find(base);
        if (it != walked.end()) return it->second;
        std::vector<std::string> files;
        walk(base.empty() ? root : root / paths::from_utf8(base), base, files);
        return walked.emplace(base, std::move(files)).first->second;
    };

    std::set<std::string> selected;
    for (const auto& raw : patterns) {
        bool exclude = raw.starts_with('!');
        std::string_view pattern = exclude ? std::string_view(raw).substr(1) : std::string_view(raw);
        for (const auto& alt : expand_braces(pattern)) {
            std::vector<std::string> segs = str::split(alt, '/');
            std::string base;
            std::size_t literal = 0;
            while (literal + 1 < segs.size() && !has_magic(segs[literal]) && segs[literal] != "**") {
                base += (base.empty() ? "" : "/") + segs[literal];
                ++literal;
            }
            std::vector<std::string> candidates;
            if (literal + 1 == segs.size() && !has_magic(segs.back()) && segs.back() != "**") {
                std::error_code ec;
                if (fs::is_regular_file(root / paths::from_utf8(alt), ec))
                    candidates.push_back(paths::to_utf8(paths::from_utf8(alt).lexically_normal()));
            } else {
                candidates = files_below(base);
            }
            for (const auto& f : candidates) {
                if (!match_parts(segs, 0, str::split(f, '/'), 0)) continue;
                if (exclude) selected.erase(f);
                else selected.insert(f);
            }
        }
    }
    return {selected.begin(), selected.end()};
}

}  // namespace cpm::glob
