#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "semver/range.hpp"
#include "toolchain/toolchain.hpp"

// Choosing the compiler (docs/spec/build.md §1.1, §1.4): requests, finding the compilers on a machine, and the order
// in which the command line, CXX, the project and the machine choose.
namespace cpm::toolchain {

namespace fs = std::filesystem;

// A family, optionally with a version range: "gcc", "clang >=18", "apple-clang 16".
struct Request {
    Family family = Family::Gcc;
    std::optional<semver::Range> range;
    std::string text;
};

// The request `text` spells, or nullopt when it doesn't start with a family name: it then names a compiler. Throws
// cpm::Error for a family followed by something that isn't a version range.
std::optional<Request> parse_request(std::string_view text);

// Whether `tc` is of the requested family and its version is in the range.
bool matches(const Toolchain& tc, const Request& request);

// Where to look for compilers, in order (build.md §1.4).
std::vector<fs::path> compiler_dirs(const fs::path& home);

// The compilers in `dirs`: g++, clang++ and versioned names such as g++-14, each program once.
std::vector<fs::path> find_compilers(const std::vector<fs::path>& dirs);

// A compiler found on this machine, probed.
struct Found {
    fs::path cxx;
    std::optional<Toolchain> toolchain;  // nullopt when probing failed
    std::string problem;                 // why CPM can't use it; empty when it can
    std::string note;                    // something to know about a usable one
};

// Every compiler on this machine (build.md §1.4), probed through the cache in $CPM_HOME/toolchains.json.
std::vector<Found> list_compilers(const fs::path& home);

// The usable compiler that best meets the request: the highest version, then the one found first. nullptr if none.
const Found* best_match(const std::vector<Found>& found, const Request& request);

// What may choose the compiler, from the command line down to the machine's configuration (build.md §1.1).
struct Choice {
    std::string text;    // a request, or a compiler name or path
    std::string source;  // "--toolchain" or "--cxx"
};

struct Selection {
    // Every member has a default so callers can use designated initializers for the ones they need.
    std::optional<Choice> command{};              // --toolchain or --cxx
    std::vector<std::string> project{};           // "toolchain" in cpm.jsonc: requests, in order of preference
    std::optional<std::string> config_cxx{};      // "cxx" in config.jsonc
    std::optional<std::string> config_toolchain{};  // "toolchain" in config.jsonc
    fs::path home{};                              // $CPM_HOME (probe cache, scratch files)
};

// Chooses the compiler (build.md §1.1) and probes it. The result's `chosen_by` says what chose it.
Toolchain find_toolchain(const Selection& selection);

}  // namespace cpm::toolchain
