#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "toolchain/language.hpp"
#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

// Probes the compiler at `cxx` (build.md §1.2), or takes the answer from $CPM_HOME/toolchains.json when the
// compiler file is unchanged (§1.3). cl.exe is probed as MSVC (msvc.hpp). Throws cpm::Error when it can't be probed.
Toolchain probe_compiler(const std::filesystem::path& cxx, const std::filesystem::path& home);

// The flag that selects `std` (e.g. "-std=c++23", or "-std=c++2b" on older compilers). The answer is
// cached in the toolchain and in toolchains.json. Throws if the compiler supports neither spelling.
std::string std_flag(Toolchain& tc, const std::string& std, const std::filesystem::path& home);

// What a compiler says about itself: its predefined macros and the lines of `-v` that matter.
struct ProbeOutput {
    std::map<std::string, std::string> macros;
    std::string target;
    std::string thread_model;
    bool apple = false;
};

// Runs `compiler -v -x <language> -E -dM` on a probe source in $CPM_HOME/tmp (abi.md §4). Throws cpm::Error when
// the compiler fails.
ProbeOutput run_probe(const std::filesystem::path& compiler, Language language, const std::filesystem::path& home);

// Whether `compiler` accepts `flag` when checking an empty source in `language`, such as a -std= spelling.
bool accepts_flag(const std::filesystem::path& compiler, Language language, const std::string& flag,
                  const std::filesystem::path& home);

// Parsing helpers, exposed for tests.
ProbeOutput parse_probe_output(std::string_view macros_text, std::string_view verbose_text);
Toolchain toolchain_from_probe(const ProbeOutput& probe, const std::filesystem::path& cxx);

// Clang targeting the MSVC ABI (*-windows-msvc, as installed by LLVM's Windows installer or Visual Studio). CPM
// supports that ABI through cl.exe, not through Clang (build.md §1.1).
bool targets_msvc(const Toolchain& tc);

}  // namespace cpm::toolchain
