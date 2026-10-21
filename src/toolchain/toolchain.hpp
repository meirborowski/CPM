#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "core/platform.hpp"

namespace cpm::toolchain {

namespace fs = std::filesystem;

enum class Family { Gcc, Clang, AppleClang, Msvc, ClangCl };

std::string_view to_string(Family family);

// A probed C++ toolchain (docs/spec/build.md §1, docs/spec/abi.md §4).
struct Toolchain {
    Family family = Family::Gcc;
    fs::path cxx;
    fs::path ar;
    std::string version;  // "13.2.0"
    std::string target;   // "x86_64-w64-mingw32"
    Os os = Os::Linux;
    std::string arch;        // x86_64 | aarch64 | x86 | arm
    std::string stdlib;      // libstdc++ | libc++ | msvc-stl
    std::string stdlib_abi;  // cxx11 | cxx98 | v1 ...
    std::string crt;         // ucrt | msvcrt | glibc | musl | libsystem
    std::string eh;          // seh | dwarf | sjlj | itanium
    std::string threads;     // posix | win32 | mcf | single
    std::map<std::string, std::string> std_flags;  // "c++23" -> "-std=c++23", filled on demand
    std::string chosen_by;                         // what chose it (build.md §1.1): "from cpm.jsonc", "found on PATH", ...

    bool is_mingw() const { return os == Os::Windows && (family == Family::Gcc || family == Family::Clang); }
    std::string description() const;  // "gcc 13.2.0"
};

}  // namespace cpm::toolchain
