// core/system_tar: which tar CPM extracts with, and the arguments each kind of tar gets (docs/spec/architecture.md
// §3.1). GNU tar, the first tar on PATH in Git Bash and MSYS2, reads C:/x as host:path unless told --force-local.
#include "core/archive.hpp"
#include "core/env.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/system_tar.hpp"
#include "support/check.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace cpm;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), fs::path(value).wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Puts a directory first on PATH for the test's duration.
struct FirstOnPath {
    std::string saved = env::get("PATH").value_or("");
    explicit FirstOnPath(const fs::path& dir) {
#ifdef _WIN32
        set_env("PATH", paths::to_utf8(dir) + ";" + saved);
#else
        set_env("PATH", paths::to_utf8(dir) + ":" + saved);
#endif
    }
    ~FirstOnPath() { set_env("PATH", saved); }
    FirstOnPath(const FirstOnPath&) = delete;
    FirstOnPath& operator=(const FirstOnPath&) = delete;
};

const std::string kSource = "int a() { return 1; }\n";

// A package as `git archive` gives it, under one top-level directory.
std::string package_tar() {
    return archive::tar({{.path = "pkg-1.0.0/src/a.cpp", .data = kSource}, {.path = "pkg-1.0.0/README.md", .data = "# pkg\n"}});
}

}  // namespace

TEST(tells_gnu_tar_from_other_tars) {
    CHECK(system_tar::is_gnu("tar (GNU tar) 1.35\nCopyright (C) 2023 Free Software Foundation, Inc.\n"));
    CHECK(!system_tar::is_gnu("bsdtar 3.7.2 - libarchive 3.7.2 zlib/1.2.13.1-motley liblzma/5.4.3 bz2lib/1.0.8 libzstd/1.5.5\n"));
    CHECK(!system_tar::is_gnu("BusyBox v1.36.1 (2024-06-10 07:11:47 UTC) multi-call binary.\n\nUsage: tar c|x|t [-ZzJjahmvokO]\n"));
    CHECK(!system_tar::is_gnu(""));
}

TEST(only_gnu_tar_is_told_that_archive_paths_are_local) {
    system_tar::Extraction package{.archive = "C:/cpm/cache/pkg.tar.gz", .dest = "C:/cpm/store/pkg", .gzip = true, .strip = 1};
    CHECK_EQ(system_tar::extract_argv({.program = "/usr/bin/tar", .gnu = true}, package),
             (std::vector<std::string>{"/usr/bin/tar", "--force-local", "-xzf", "C:/cpm/cache/pkg.tar.gz", "-C", "C:/cpm/store/pkg",
                                       "--strip-components=1"}));
    // bsdtar rejects --force-local, and never reads a path as host:path.
    CHECK_EQ(system_tar::extract_argv({.program = "C:/Windows/System32/tar.exe"}, package),
             (std::vector<std::string>{"C:/Windows/System32/tar.exe", "-xzf", "C:/cpm/cache/pkg.tar.gz", "-C", "C:/cpm/store/pkg",
                                       "--strip-components=1"}));

    system_tar::Extraction plain{.archive = "C:/cpm/tmp/src.tar", .dest = "C:/cpm/tmp/pkg"};
    CHECK_EQ(system_tar::extract_argv({.program = "tar", .gnu = true}, plain),
             (std::vector<std::string>{"tar", "--force-local", "-xf", "C:/cpm/tmp/src.tar", "-C", "C:/cpm/tmp/pkg"}));
}

#ifdef _WIN32
TEST(extracts_with_the_tar_windows_ships_even_when_another_comes_first_on_path) {
    // As in Git Bash, where GNU tar comes first on PATH. A stand-in does here: locating a program never runs it.
    check::TempDir dir;
    fsx::write_file_atomic(dir.path() / "tar.exe", "");
    FirstOnPath first(dir.path());
    CHECK(paths::same_file(cpm::proc::find_program("tar").value_or(""), dir.path() / "tar.exe"));
    fs::path windows_tar = paths::from_utf8(env::get("SystemRoot").value_or("C:\\Windows")) / "System32" / "tar.exe";
    CHECK(paths::same_file(system_tar::locate().value_or(""), windows_tar));
}
#else
TEST(extracts_with_the_first_tar_on_path) {
    check::TempDir dir;
    fsx::write_file_atomic(dir.path() / "tar", "#!/bin/sh\n");
    fs::permissions(dir.path() / "tar", fs::perms::owner_exec, fs::perm_options::add);
    FirstOnPath first(dir.path());
    CHECK(paths::same_file(system_tar::locate().value_or(""), dir.path() / "tar"));
}
#endif

// Every tar on this machine, given CPM's arguments for it, extracts from and into absolute paths, which start with
// a drive letter on Windows.
TEST(every_tar_here_extracts_absolute_paths) {
    std::vector<fs::path> tars;
    for (const auto& tar : {system_tar::locate(), cpm::proc::find_program("tar")}) {
        if (tar) tars.push_back(*tar);
    }
#ifdef _WIN32
    // Git for Windows' GNU tar, the first tar on PATH in Git Bash.
    if (auto program_files = env::get("ProgramFiles")) {
        if (auto git_tar = cpm::proc::find_program(*program_files + "\\Git\\usr\\bin\\tar.exe")) tars.push_back(*git_tar);
    }
#endif
    CHECK(!tars.empty());

    check::TempDir dir;
    fs::path file = dir.path() / "src.tar";  // uncompressed: GNU tar runs gzip from PATH, which may not have it
    fsx::write_file_atomic(file, package_tar());
    for (std::size_t i = 0; i < tars.size(); ++i) {
        system_tar::Tar tar = system_tar::probe(tars[i]);
        fs::path out = dir.path() / std::format("out-{}", i);
        fsx::create_dirs(out);
        cpm::proc::Captured r = cpm::proc::capture({.argv = system_tar::extract_argv(tar, {.archive = file, .dest = out, .strip = 1})});
        if (r.exit_code != 0) check::fail(__FILE__, __LINE__, std::format("{} failed:\n{}", paths::to_utf8(tars[i]), r.err));
        CHECK_EQ(fsx::try_read_file(out / "src" / "a.cpp").value_or(""), kSource);
    }
}

TEST(extract_creates_the_destination_and_names_what_failed) {
    check::TempDir dir;
    fs::path file = dir.path() / "pkg.tar.gz";
    fsx::write_file_atomic(file, archive::gzip(package_tar()));
    system_tar::extract({.archive = file, .dest = dir.path() / "new" / "pkg", .gzip = true, .strip = 1}, "pkg@1.0.0");
    CHECK_EQ(fsx::read_file(dir.path() / "new" / "pkg" / "src" / "a.cpp"), kSource);

    fsx::write_file_atomic(dir.path() / "broken.tar.gz", "not an archive");
    CHECK_THROWS(system_tar::extract({.archive = dir.path() / "broken.tar.gz", .dest = dir.path() / "broken", .gzip = true}, "broken@1.0.0"),
                 "cannot extract broken@1.0.0");
}
