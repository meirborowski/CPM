#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/platform.hpp"
#include "json/value.hpp"
#include "manifest/profile.hpp"
#include "semver/version.hpp"

namespace cpm::manifest {

namespace fs = std::filesystem;

enum class TargetKind { Executable, StaticLib, SharedLib, HeaderOnly };

std::string_view to_string(TargetKind kind);
std::optional<TargetKind> parse_target_kind(std::string_view text);

// One entry of "dependencies" / "devDependencies" (docs/spec/dependencies.md §1).
struct DependencySpec {
    enum class Kind { Registry, Git, Path };
    std::string name;
    Kind kind = Kind::Registry;
    std::string specifier;  // verbatim string, or canonical JSON for the object form
    std::string range;      // Registry: the range text (parsed by the resolver)
    std::string registry;   // Registry: registry name; empty = "default"
    std::string git_url;    // Git
    std::string git_ref;    // Git: tag, branch or commit; empty = default branch
    std::string path;       // Path, as written
    std::optional<json::Value> overlay;
    bool dev = false;
    json::Span span;
};

struct PlatformOverrides {
    std::vector<std::string> defines, public_defines, system_libs, sources, cxxflags, ldflags;
};

// A loaded cpm.jsonc (docs/spec/manifest.md §3).
struct Manifest {
    fs::path file;  // absolute path of cpm.jsonc
    fs::path root;  // its directory
    std::string text;

    std::string name;
    semver::Version version;
    std::string description, license, repository, homepage;
    std::vector<std::string> authors, keywords;
    bool is_private = false;
    std::string std = "c++20";
    std::optional<TargetKind> type;
    std::vector<std::string> toolchain;  // compiler requests, in order of preference (build.md §1.1)
    std::optional<std::string> main;
    std::vector<std::string> sources;  // empty = default globs
    std::string include_dir = "include";
    std::vector<std::string> defines, public_defines, system_libs, files;
    std::vector<std::pair<std::string, std::string>> scripts;
    std::vector<DependencySpec> dependencies, dev_dependencies;
    std::map<std::string, std::string> overrides;  // "fmt" or "spdlog>fmt" -> range (dependencies.md §4.6)
    std::map<std::string, Profile> profiles;
    std::map<std::string, PlatformOverrides> platforms;  // windows | linux | macos | unix

    std::vector<Diagnostic> warnings;
};

inline const std::vector<std::string>& default_sources() {
    static const std::vector<std::string> globs = {"src/**/*.cpp", "src/**/*.cc", "src/**/*.cxx"};
    return globs;
}

inline const std::vector<std::string>& known_standards() {
    static const std::vector<std::string> stds = {"c++11", "c++14", "c++17", "c++20", "c++23", "c++26"};
    return stds;
}

bool valid_package_name(std::string_view name);

// The override for `dependent`'s requirement on `name` (dependencies.md §4.6): the scoped "dependent>name" entry, else
// the plain "name" one. Returns the entry (key and range), or nullptr.
const std::pair<const std::string, std::string>* find_override(const std::map<std::string, std::string>& overrides,
                                                               std::string_view dependent, std::string_view name);

// The override that replaces a package's requirement, from the project's manifest. Requirements that name a git or
// path source have no range, so nothing replaces them.
const std::pair<const std::string, std::string>* override_for(const Manifest& project, std::string_view dependent,
                                                              const DependencySpec& requirement);

// Parses manifest text. `file` is the manifest's path; `display` is how to name it in diagnostics.
// Throws DiagnosticsError listing every problem found.
Manifest parse(std::string text, const fs::path& file, std::string display);

// Reads and parses <dir>/cpm.jsonc.
Manifest load(const fs::path& file, const fs::path& display_base);

// The effective manifest of a dependency (manifest.md §8): its own cpm.jsonc (`text`, absent if it has
// none) with the overlay's top-level fields replacing its own. `name` is the dependency's name;
// `fallback_version` is used when neither the overlay nor the package gives a version. Throws if the
// package calls itself something else.
Manifest parse_package(const std::optional<std::string>& text, const std::optional<json::Value>& overlay,
                       const std::string& name, const std::string& fallback_version, const fs::path& file,
                       const std::string& display);

// The platform additions that apply to `os`: "unix" (for non-Windows) followed by the OS's own entry.
PlatformOverrides effective_platform(const Manifest& m, Os os);

}  // namespace cpm::manifest
