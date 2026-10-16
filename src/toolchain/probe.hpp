#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

struct Selection {
    std::optional<std::string> cli_cxx;     // --cxx
    std::optional<std::string> config_cxx;  // "cxx" in config.jsonc
    std::filesystem::path home;             // $CPM_HOME (probe cache, scratch files)
};

// Picks the compiler (build.md §1.1) and probes it, using $CPM_HOME/toolchains.json as a cache.
Toolchain find_toolchain(const Selection& selection);

// The flag that selects `std` (e.g. "-std=c++23", or "-std=c++2b" on older compilers). The answer is
// cached in the toolchain and in toolchains.json. Throws if the compiler supports neither spelling.
std::string std_flag(Toolchain& tc, const std::string& std, const std::filesystem::path& home);

// Parsing helpers, exposed for tests.
struct ProbeOutput {
    std::map<std::string, std::string> macros;
    std::string target;
    std::string thread_model;
    bool apple = false;
};
ProbeOutput parse_probe_output(std::string_view macros_text, std::string_view verbose_text);
Toolchain toolchain_from_probe(const ProbeOutput& probe, const std::filesystem::path& cxx);

// Clang targeting the MSVC ABI (*-windows-msvc, as installed by LLVM's Windows installer or Visual Studio). It
// links like MSVC (.lib files, lld-link), which needs the MSVC driver (post-MVP).
bool targets_msvc(const Toolchain& tc);

}  // namespace cpm::toolchain
