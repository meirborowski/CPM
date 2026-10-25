#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

// The file name of the C compiler that goes with a C++ compiler's (docs/spec/build.md §1.2): clang++ -> clang,
// g++ -> gcc, c++ -> cc, keeping any prefix and suffix ("x86_64-w64-mingw32-g++-13.exe" ->
// "x86_64-w64-mingw32-gcc-13.exe"). nullopt when the name contains none of them.
std::optional<std::string> c_compiler_name(std::string_view cxx_file_name);

// Finds the C compiler next to tc.cxx and checks that it reports the same family, version and target, so that C and
// C++ objects come from one toolchain (build.md §1.2). Taken from toolchains.json while the C compiler file is
// unchanged. Sets tc.c. Throws cpm::Error when the C compiler is missing or reports something else.
void find_c_compiler(Toolchain& tc, const std::filesystem::path& home);

// The flag that selects `cstd` ("-std=gnu17", or "-std=c2x" for c23 on compilers older than that name), finding the
// C compiler first. Cached in the toolchain and in toolchains.json. Throws if the C compiler supports no spelling.
std::string cstd_flag(Toolchain& tc, const std::string& cstd, const std::filesystem::path& home);

}  // namespace cpm::toolchain
