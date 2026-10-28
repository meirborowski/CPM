#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "build/layout.hpp"
#include "manifest/manifest.hpp"
#include "toolchain/driver.hpp"
#include "toolchain/language.hpp"
#include "toolchain/toolchain.hpp"

namespace cpm::build {

// All paths in a plan are generic ('/') and relative to the project root unless they lie outside it.

struct CompileStep {
    enum class Kind { Core, Entry, Test, Dependency };
    std::string source;
    std::string object;
    Kind kind = Kind::Core;
    toolchain::Language language = toolchain::Language::Cxx;
    // Kind::Dependency only: a path dependency's own include and definition flags (replacing the root's), and for
    // C its own C flags, which carry its own standard (build.md §4.4).
    std::vector<std::string> incs{}, defines{}, cflags{};
};

struct ArchiveStep {
    std::string output;
    std::vector<std::string> members;
};

struct LinkStep {
    std::string name;                        // phony target name ("hello", "json.test")
    std::string output;
    std::vector<std::string> inputs;         // objects and archives
    std::vector<std::string> implicit;       // extra dependencies (libraries linked via flags)
    std::vector<std::string> implicit_outputs;  // extra files the link writes (MinGW import library)
    std::vector<std::string> extra_ldflags;  // per-edge additions to $ldflags
    std::vector<std::string> extra_libs;     // per-edge additions in front of $libs
    bool shared = false;
};

// A resolved dependency as the build sees it (produced by the installer). A list of them is in
// dependency order: every package before the packages it depends on (build.md §4.3).
struct Dependency {
    std::string name;
    std::string version;
    std::string identity;             // exactly what is linked (integrity, commit or path); see dependency_stamp
    bool dev = false;                 // reachable only through devDependencies: used by tests only
    std::string include_dir;          // "cpm_modules/<name>/include", or empty
    std::string lib_dir;              // "cpm_modules/<name>/lib" for a library from the store, or empty
    std::string link_name;            // linked as -l<link_name> from lib_dir
    std::string lib_file;             // the archive in lib_dir: an implicit input of every link
    std::vector<std::string> public_defines;
    std::vector<std::string> system_libs;

    // A path dependency is compiled inside this project's graph (build.md §4.4).
    struct Sources {
        std::string root;                       // the package directory, relative to the project root
        std::vector<std::string> files;         // relative to `root`
        std::vector<std::string> include_dirs;  // relative to the project root
        std::vector<std::string> defines;       // its defines, publicDefines and its dependencies' publicDefines
        std::string cstd = "gnu17";             // the standard of its C files (manifest.md §3.3)
    };
    std::optional<Sources> sources;
};

struct BuildPlan {
    std::filesystem::path root;
    std::string build_dir;  // intermediates: "cpm_modules/.cache/<profile>" (build.ninja, obj/, logs)
    std::string out_dir;    // outputs: "build/<profile>" (executable, libraries, tests/)
    std::string profile;
    std::string package;
    std::string package_version;
    manifest::TargetKind kind = manifest::TargetKind::Executable;
    toolchain::Toolchain toolchain;

    std::vector<std::string> cxxflags, cflags, defines, incs, test_incs, test_defines, ldflags, libs;  // ready-made flags
    // A library root's own export macro on its sources (manifest.md §4.1); `defines` holds what every root compile
    // shares, and `test_defines` adds the tests' own, the root's export macro among them.
    std::vector<std::string> source_defines;
    std::vector<CompileStep> compiles;
    std::optional<ArchiveStep> archive;  // static library
    std::vector<ArchiveStep> dependency_archives;  // path dependencies
    std::optional<LinkStep> main;        // executable or shared library
    std::vector<LinkStep> tests;
    std::string main_output;             // the package's artifact; empty for header-only
    // A file whose content names every dependency's identity. Every compile and link depends on it, because
    // cpm_modules links can point at different files under the same paths (another version, an older mtime),
    // which Ninja's timestamps alone would miss. Empty when there are no dependencies.
    std::string dependency_stamp;
    std::string dependency_stamp_text;
    std::vector<std::string> notes;
};

// The flags that select the standards a plan compiles with, spelled as the compiler accepts them: the project's C++
// standard, and each C standard that c_standards() lists ("gnu17" -> "-std=gnu17").
struct StandardFlags {
    std::string cxx;
    std::map<std::string, std::string> c{};
};

// The C standards a build compiles with (manifest.md §3.3): the root's when it has C sources, and each path
// dependency's that has C sources. Sorted, without duplicates; empty when nothing is C.
std::vector<std::string> c_standards(const manifest::Manifest& m, const Layout& layout,
                                     const std::vector<Dependency>& dependencies);

// Turns a manifest, a resolved profile and a probed toolchain into a build plan (docs/spec/build.md §4). When
// anything is C, tc.c is the C compiler and `standards.c` has a flag for every entry of c_standards().
BuildPlan make_plan(const manifest::Manifest& m, const manifest::Profile& profile, const toolchain::Toolchain& tc,
                    const StandardFlags& standards, const toolchain::Driver& driver, const Layout& layout,
                    const std::vector<Dependency>& dependencies = {});

// Whether the plan compiles any C, and so needs the C compiler.
bool compiles_c(const BuildPlan& plan);

// Object file path for a source, inside `build_dir`/obj. Escapes ".." and drive letters.
std::string object_path(const std::string& build_dir, const std::string& source, const std::string& ext);

}  // namespace cpm::build
