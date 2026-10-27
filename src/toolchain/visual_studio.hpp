#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "toolchain/toolchain.hpp"

// Visual Studio and its Build Tools (docs/spec/build.md §1.4, §1.5): the installations vswhere reports, the MSVC
// toolsets each one has, and the environment vcvarsall.bat sets for a toolset. The parsers are pure; the rest runs
// only on Windows and finds nothing elsewhere.
namespace cpm::toolchain::vs {

namespace fs = std::filesystem;

// The installations described by vswhere's JSON output (-format json). Throws cpm::Error when it isn't JSON.
std::vector<VisualStudio> parse_vswhere(std::string_view json_text);

// The installations on this machine that have the C++ tools, from vswhere. Its answer is cached in
// $CPM_HOME/toolchains.json until the Visual Studio Installer's instance directory changes.
std::vector<VisualStudio> installations(const fs::path& home);

// The architecture MSVC builds for on this machine: "x64", or "arm64" on ARM64 Windows. Also vcvarsall.bat's
// argument, and the name of the directories under a toolset's bin/.
std::string host_arch();

// The directories with cl.exe of each toolset of `vs`, newest toolset first: VC/Tools/MSVC/<toolset>/bin/Hostx64/x64.
std::vector<fs::path> compiler_dirs(const VisualStudio& vs);

// The installation and toolset a cl.exe belongs to, from where it is (…/VC/Tools/MSVC/<toolset>/bin/Host<arch>/<arch>/
// cl.exe), with the name and version from `known` when the installation is one of them. nullopt when cl.exe isn't
// inside a Visual Studio.
std::optional<VisualStudio> locate(const fs::path& cl, const std::vector<VisualStudio>& known);

// What vcvarsall.bat set, from the output of `set` after it ran: INCLUDE, LIB, LIBPATH, and as PATH the directories
// it added to `inherited_path` (';'-separated, in order).
std::map<std::string, std::string> parse_environment(std::string_view set_output, std::string_view inherited_path);

// Runs vcvarsall.bat for the toolset of `vs` and returns what it set (build.md §1.5). Throws cpm::Error when it fails.
std::map<std::string, std::string> capture_environment(const VisualStudio& vs);

// Whether every INCLUDE and LIB directory of a captured environment still exists.
bool environment_intact(const std::map<std::string, std::string>& env);

// `env` as variables for a child process: INCLUDE, LIB and LIBPATH as captured, and PATH with the captured
// directories in front of `inherited_path`.
std::vector<std::pair<std::string, std::string>> process_variables(const std::map<std::string, std::string>& env,
                                                                   std::string_view inherited_path);

// The Ninja an installation ships with its CMake tools, if any has one (build.md §6).
std::optional<fs::path> bundled_ninja(const fs::path& home);

}  // namespace cpm::toolchain::vs
