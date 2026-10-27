#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "toolchain/toolchain.hpp"

// Visual Studio's cl.exe (docs/spec/build.md §1.3-§3, abi.md §4): probing it, and its /std: flags. The parsers are
// pure; probing runs cl.exe in its environment (build.md §1.5).
namespace cpm::toolchain::msvc {

namespace fs = std::filesystem;

// Whether `compiler` is cl.exe, by its name.
bool is_cl(const fs::path& compiler);

// The macros the probe printed after its markers, in `cl /EP` output: "_MSC_FULL_VER" -> "195136260". Macros the
// compiler doesn't define are left out.
std::map<std::string, std::string> parse_probe(std::string_view preprocessed);

// The toolchain the probe's macros describe. Throws cpm::Error when they aren't MSVC's.
Toolchain toolchain_from_probe(const std::map<std::string, std::string>& macros, const fs::path& cl);

// _MSC_FULL_VER as a version, and back: "195136260" <-> "19.51.36260".
std::string display_version(std::string_view full_ver);
std::string full_version(std::string_view version);

// The text before the header's path in /showIncludes output: "Note: including file:", or its translation. nullopt
// when no line names a file in `dir`.
std::optional<std::string> parse_deps_prefix(std::string_view output, const fs::path& dir);

// Probes cl.exe: where it comes from, its environment, its macros and its /showIncludes prefix. Throws cpm::Error.
Toolchain probe(const fs::path& cl, const fs::path& home);

// Whether cl.exe accepts `flag`. It answers a flag it doesn't know with warning D9002 and still succeeds.
bool accepts(const Toolchain& tc, const std::string& flag, const fs::path& home);

// The /std: flag for a C++ standard (build.md §3), found the first time and cached. Throws when there is none.
std::string std_flag(Toolchain& tc, const std::string& std, const fs::path& home);

// The /std: flag for a C standard, or "" for c89 and c99, which are MSVC's default C. Throws for an unknown one.
std::string cstd_flag(const std::string& cstd);

}  // namespace cpm::toolchain::msvc
