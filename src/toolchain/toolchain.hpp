#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/platform.hpp"

namespace cpm::toolchain {

namespace fs = std::filesystem;

enum class Family { Gcc, Clang, AppleClang, Msvc, ClangCl };

std::string_view to_string(Family family);

// The C compiler that goes with a C++ compiler (docs/spec/build.md §1.2).
struct CCompiler {
    fs::path path;
    std::map<std::string, std::string> cstd_flags{};  // "c23" -> "-std=c2x", filled on demand
};

// The Visual Studio (or Build Tools) installation and toolset an MSVC compiler comes from (build.md §1.4).
struct VisualStudio {
    fs::path path;        // the installation: "C:/Program Files/Microsoft Visual Studio/18/Community"
    std::string name;     // "Visual Studio Community 2026"
    std::string version;  // "18.11.0"
    std::string toolset;  // "14.51.36231": the directory under VC/Tools/MSVC
};

// A probed C++ toolchain (docs/spec/build.md §1, docs/spec/abi.md §4).
struct Toolchain {
    Family family = Family::Gcc;
    fs::path cxx;
    fs::path ar;
    fs::path linker;      // MSVC: link.exe. GCC and Clang link with the compiler.
    std::string version;  // "13.2.0"; MSVC: "19.51.36260"
    std::string target;   // "x86_64-w64-mingw32"
    Os os = Os::Linux;
    std::string arch;        // x86_64 | aarch64 | x86 | arm
    std::string stdlib;      // libstdc++ | libc++ | msvc-stl
    std::string stdlib_abi;  // cxx11 | cxx98 | v1 ...
    std::string crt;         // ucrt | msvcrt | glibc | musl | libsystem
    std::string eh;          // seh | dwarf | sjlj | itanium
    std::string threads;     // posix | win32 | mcf | single
    std::map<std::string, std::string> std_flags;  // "c++23" -> "-std=c++23", filled on demand
    std::optional<CCompiler> c;                    // found when a build first compiles C (find_c_compiler)
    std::string chosen_by;                         // what chose it (build.md §1.1): "from cpm.jsonc", "found on PATH", ...

    // MSVC only (build.md §1.3, §1.5).
    std::optional<VisualStudio> visual_studio;
    std::map<std::string, std::string> env;  // INCLUDE, LIB, LIBPATH, EXTERNAL_INCLUDE, and PATH: what vcvarsall.bat adds
    std::string deps_prefix;                 // what starts /showIncludes lines: "Note: including file:"

    bool is_mingw() const { return os == Os::Windows && (family == Family::Gcc || family == Family::Clang); }
    std::string description() const;  // "gcc 13.2.0"
};

// The variables a process that runs the toolchain's tools needs: for MSVC, INCLUDE, LIB, LIBPATH, EXTERNAL_INCLUDE and
// PATH with the toolchain's directories in front of the inherited PATH (build.md §1.5). Nothing for GCC and Clang.
std::vector<std::pair<std::string, std::string>> process_environment(const Toolchain& tc);

// The directories whose DLLs programs built by the toolchain load at run time on Windows: MinGW's compiler
// directory (libstdc++, libgcc, winpthread), or MSVC's PATH directories (the AddressSanitizer runtime). Empty
// elsewhere.
std::vector<fs::path> runtime_dirs(const Toolchain& tc);

}  // namespace cpm::toolchain
