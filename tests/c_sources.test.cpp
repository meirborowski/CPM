// C sources (ROADMAP.md 0.4.5): which files are C, the C compiler that goes with the C++ compiler, each package's own
// C standard, and real builds of C programs, mixed programs and C libraries from a path, a registry and git.

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "build/layout.hpp"
#include "build/ninja_gen.hpp"
#include "build/pipeline.hpp"
#include "build/plan.hpp"
#include "core/env.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "install/installer.hpp"
#include "lock/lockfile.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"
#include "toolchain/c_compiler.hpp"
#include "toolchain/language.hpp"
#include "toolchain/select.hpp"

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
using manifest::TargetKind;
using toolchain::Language;

namespace {

void set_env(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void touch(const fs::path& p) { fixture::write(p, "/* test */\n"); }

manifest::Manifest make_manifest(const fs::path& root, const std::string& extra = "") {
    return manifest::parse(R"({ "name": "hello", "version": "1.0.0")" + extra + " }", root / "cpm.jsonc", "cpm.jsonc");
}

toolchain::Toolchain fake_linux_gcc() {
    toolchain::Toolchain tc;
    tc.family = toolchain::Family::Gcc;
    tc.cxx = "/usr/bin/g++";
    tc.ar = "/usr/bin/gcc-ar";
    tc.version = "13.2.0";
    tc.target = "x86_64-linux-gnu";
    tc.os = Os::Linux;
    tc.c = toolchain::CCompiler{.path = "/usr/bin/gcc"};
    return tc;
}

// Builds the project at `dir` with its tests, installing its dependencies first.
build::BuildResult build_with_tests(const fs::path& dir, const manifest::UserConfig& config = {}) {
    build::BuildRequest req;
    req.cwd = dir;
    req.targets = build::BuildRequest::Targets::MainAndTests;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    return build::run_build(req, config);
}

std::string first_line(const std::string& text) { return text.substr(0, text.find_first_of("\r\n")); }

std::string run_program(const fs::path& exe) { return first_line(cpm::proc::capture({.argv = {paths::to_utf8(exe)}}).out); }

}  // namespace

TEST(c_files_are_c_and_everything_else_is_cpp) {
    CHECK(toolchain::language_of("src/parse.c") == Language::C);
    CHECK(toolchain::language_of("tests/parse.test.c") == Language::C);
    for (const char* cxx : {"src/main.cpp", "src/a.cc", "src/b.cxx", "src/old.C"}) {
        INFO("{}", cxx);
        CHECK(toolchain::language_of(cxx) == Language::Cxx);
    }
}

TEST(the_c_compiler_is_named_after_the_cpp_compiler) {
    CHECK_EQ(toolchain::c_compiler_name("g++").value_or(""), "gcc");
    CHECK_EQ(toolchain::c_compiler_name("g++-14").value_or(""), "gcc-14");
    CHECK_EQ(toolchain::c_compiler_name("g++.exe").value_or(""), "gcc.exe");
    CHECK_EQ(toolchain::c_compiler_name("x86_64-w64-mingw32-g++-13.exe").value_or(""), "x86_64-w64-mingw32-gcc-13.exe");
    CHECK_EQ(toolchain::c_compiler_name("clang++").value_or(""), "clang");
    CHECK_EQ(toolchain::c_compiler_name("clang++-18").value_or(""), "clang-18");
    CHECK_EQ(toolchain::c_compiler_name("c++").value_or(""), "cc");
    CHECK(!toolchain::c_compiler_name("cl.exe").has_value());
}

TEST(each_package_has_its_own_c_standard) {
    check::TempDir dir;
    auto plain = make_manifest(dir.path());
    CHECK_EQ(plain.cstd, "gnu17");
    CHECK_EQ(plain.std, "c++20");

    // A C library needs no particular C++ standard, but a project still compiles C++ with c++20.
    auto clib = make_manifest(dir.path(), R"(, "cstd": "c99")");
    CHECK_EQ(clib.cstd, "c99");
    CHECK_EQ(clib.std, "c++11");
    CHECK_EQ(manifest::project_std(clib), "c++20");

    auto both = make_manifest(dir.path(), R"(, "cstd": "c11", "std": "c++17")");
    CHECK_EQ(both.std, "c++17");
    CHECK_EQ(manifest::project_std(both), "c++17");

    CHECK_THROWS(make_manifest(dir.path(), R"(, "cstd": "c18")"), "unknown C standard \"c18\"");

    // An overlay can set it too.
    json::Value overlay = json::Value::object();
    overlay.set("type", "static-lib");
    overlay.set("cstd", "gnu99");
    auto z = manifest::parse_package(std::nullopt, overlay, "z", "1.0.0", dir.path() / "cpm.jsonc", "z");
    CHECK_EQ(z.cstd, "gnu99");
    CHECK_EQ(z.std, "c++11");
}

TEST(cflags_are_profile_and_platform_settings) {
    check::TempDir dir;
    auto m = make_manifest(dir.path(), R"(, "profiles": { "debug": { "cflags": ["-fno-common"] },
        "tight": { "inherits": "debug", "cflags": ["-fstack-protector"] } },
        "platforms": { "unix": { "cflags": ["-pthread"] }, "linux": { "cflags": ["-D_GNU_SOURCE"] } })");
    CHECK_EQ(m.profiles.at("tight").cflags, (std::vector<std::string>{"-fno-common", "-fstack-protector"}));
    CHECK(m.profiles.at("tight").cxxflags.empty());
    CHECK_EQ(manifest::effective_platform(m, Os::Linux).cflags, (std::vector<std::string>{"-pthread", "-D_GNU_SOURCE"}));
    CHECK(manifest::effective_platform(m, Os::Windows).cflags.empty());
}

TEST(layout_finds_c_entrypoints_sources_and_tests) {
    {
        check::TempDir dir;
        for (const char* f : {"src/main.c", "src/util.c", "src/fast.cpp", "tests/util.test.c", "tests/fast.test.cpp"})
            touch(dir.path() / f);
        auto layout = build::infer_layout(make_manifest(dir.path()), {});
        CHECK(layout.kind == TargetKind::Executable);
        CHECK_EQ(layout.entry.value_or(""), "src/main.c");
        CHECK_EQ(layout.sources, (std::vector<std::string>{"src/fast.cpp", "src/util.c"}));
        CHECK_EQ(layout.tests, (std::vector<std::string>{"tests/fast.test.cpp", "tests/util.test.c"}));
    }
    {
        check::TempDir dir;
        touch(dir.path() / "src/lib.c");
        auto layout = build::infer_layout(make_manifest(dir.path()), {});
        CHECK(layout.kind == TargetKind::StaticLib);
        CHECK_EQ(layout.entry.value_or(""), "src/lib.c");
    }
    {
        check::TempDir dir;  // C++ comes first when both exist
        touch(dir.path() / "src/main.cpp");
        touch(dir.path() / "src/main.c");
        auto layout = build::infer_layout(make_manifest(dir.path()), {});
        CHECK_EQ(layout.entry.value_or(""), "src/main.cpp");
    }
}

TEST(c_warnings_leave_out_cpp_only_flags) {
    auto driver = toolchain::make_driver(fake_linux_gcc());
    manifest::Profile p;
    p.warnings = "strict";
    auto c = driver->profile_flags(p, Language::C, "-std=gnu17", false).compile;
    auto cxx = driver->profile_flags(p, Language::Cxx, "-std=c++20", false).compile;
    CHECK_EQ(c.front(), "-std=gnu17");
    CHECK(std::find(c.begin(), c.end(), "-Wconversion") != c.end());
    CHECK(std::find(c.begin(), c.end(), "-Wnon-virtual-dtor") == c.end());
    CHECK(std::find(cxx.begin(), cxx.end(), "-Wnon-virtual-dtor") != cxx.end());
}

TEST(build_ninja_compiles_c_with_the_c_compiler) {
    check::TempDir dir;
    for (const char* f : {"src/main.cpp", "src/parse.c"}) touch(dir.path() / f);
    auto m = make_manifest(dir.path(), R"(, "profiles": { "debug": { "cflags": ["-fno-common"] } })");
    auto tc = fake_linux_gcc();
    auto driver = toolchain::make_driver(tc);
    auto layout = build::infer_layout(m, {});

    // A path dependency with C of its own standard.
    build::Dependency dep;
    dep.name = "clib";
    dep.version = "0.1.0";
    dep.identity = "path:../clib";
    dep.sources = build::Dependency::Sources{.root = "../clib", .files = {"src/lib.c"}, .include_dirs = {"../clib/include"},
                                             .defines = {}, .cstd = "c99"};
    CHECK_EQ(build::c_standards(m, layout, {dep}), (std::vector<std::string>{"c99", "gnu17"}));

    build::StandardFlags standards{.cxx = "-std=c++20", .c = {{"gnu17", "-std=gnu17"}, {"c99", "-std=c99"}}};
    auto plan = build::make_plan(m, m.profiles.at("debug"), tc, standards, *driver, layout, {dep});
    CHECK(build::compiles_c(plan));
    std::string ninja = build::generate_ninja(plan, *driver);
    CHECK(ninja.contains("\ncc = /usr/bin/gcc\n"));
    CHECK(ninja.contains("\ncflags = -std=gnu17 -O0 -g -Wall -Wextra -fdiagnostics-color=always -fno-common\n"));
    CHECK(ninja.contains("\nrule cc\n  command = $cc -MD -MF $out.d $cflags $defines $incs -c $in -o $out\n"));
    CHECK(ninja.contains("obj/src/parse.c.o: cc $\n    src/parse.c $\n"));  // one input per line with a dependency
    CHECK(ninja.contains("obj/src/main.cpp.o: cxx $\n    src/main.cpp $\n"));
    CHECK(ninja.contains("  cflags = -std=c99 -O0 -g -Wall -Wextra -fdiagnostics-color=always -fno-common\n"));
    CHECK(ninja.contains("command = $cxx @$out.rsp -o $out $ldflags"));  // links stay on the C++ driver

    // Without C, nothing about C appears.
    check::TempDir plain_dir;
    touch(plain_dir.path() / "src/main.cpp");
    auto plain = make_manifest(plain_dir.path());
    auto plain_layout = build::infer_layout(plain, {});
    CHECK(build::c_standards(plain, plain_layout, {}).empty());
    auto plain_plan = build::make_plan(plain, plain.profiles.at("debug"), tc, {.cxx = "-std=c++20"}, *driver, plain_layout);
    std::string plain_ninja = build::generate_ninja(plain_plan, *driver);
    CHECK(!plain_ninja.contains("cc ="));
    CHECK(!plain_ninja.contains("cflags"));
    CHECK(!plain_ninja.contains("rule cc"));
}

TEST(a_missing_or_different_c_compiler_is_an_error) {
    check::TempDir t;
    toolchain::Toolchain lonely = fake_linux_gcc();
    lonely.c.reset();
    lonely.cxx = t.path() / "bin" / "g++";
    touch(lonely.cxx);
    CHECK_THROWS(toolchain::find_c_compiler(lonely, t.path() / "home"), "there is no C compiler next to");

    // The real compiler's companion, checked against a C++ compiler that claims another version.
    toolchain::Toolchain real = toolchain::find_toolchain({.home = t.path() / "home"});
    real.c.reset();
    real.version = "0.0.1";
    CHECK_THROWS(toolchain::find_c_compiler(real, t.path() / "home2"), "but the C++ compiler");
}

TEST(builds_and_tests_a_pure_c_program) {
    check::TempDir t;
    set_env("CPM_HOME", t.path() / "home");
    fs::path app = t.path() / "capp";
    fixture::write(app / "cpm.jsonc", R"({ "name": "capp", "version": "1.0.0", "cstd": "c11" })");
    fixture::write(app / "src" / "add.h", "#ifndef ADD_H\n#define ADD_H\nint add(int a, int b);\n#endif\n");
    fixture::write(app / "src" / "add.c", "#include \"add.h\"\nint add(int a, int b) { return a + b; }\n");
    fixture::write(app / "src" / "main.c",
                   "#ifdef __cplusplus\n#error compiled as C++\n#endif\n"
                   "#if __STDC_VERSION__ != 201112L\n#error not C11\n#endif\n"
                   "#include <stdio.h>\n#include \"add.h\"\nint main(void) { printf(\"%d\\n\", add(2, 3)); return 0; }\n");
    fixture::write(app / "tests" / "add.test.c", "#include \"add.h\"\nint main(void) { return add(2, 3) == 5 ? 0 : 1; }\n");

    build::BuildResult built = build_with_tests(app);
    CHECK_EQ(built.exit_code, 0);
    CHECK_EQ(run_program(app / built.plan.main_output), "5");
    CHECK_EQ(built.plan.tests.size(), 1u);
    if (built.plan.tests.size() == 1) {
        CHECK_EQ(cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.tests[0].output)}}).exit_code, 0);
    }
    CHECK(fsx::read_file(app / "compile_commands.json").contains("add.c"));
}

TEST(a_cpp_program_uses_its_own_c_and_a_c_library_with_another_standard) {
    check::TempDir t;
    set_env("CPM_HOME", t.path() / "home");

    // A C library on C99. It sets no "std", so a C++17 project can use it.
    fs::path clib = t.path() / "clib";
    fixture::write(clib / "cpm.jsonc", R"({ "name": "clib", "version": "0.1.0", "cstd": "c99" })");
    fixture::write(clib / "include" / "clib" / "clib.h",
                   "#ifndef CLIB_H\n#define CLIB_H\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nint clib_twice(int x);\n"
                   "#ifdef __cplusplus\n}\n#endif\n#endif\n");
    fixture::write(clib / "src" / "lib.c",
                   "#if __STDC_VERSION__ != 199901L\n#error not C99\n#endif\n"
                   "#include <clib/clib.h>\nint clib_twice(int x) { return 2 * x; }\n");

    // A C++17 program with a C file of its own, compiled with the default gnu17.
    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++17" })");
    fixture::write(app / "src" / "helper.h", "#pragma once\nextern \"C\" int helper(int);\n");
    fixture::write(app / "src" / "helper.c",
                   "#if __STDC_VERSION__ != 201710L || defined(__STRICT_ANSI__)\n#error not gnu17\n#endif\n"
                   "int helper(int x) { return x + 1; }\n");
    fixture::write(app / "src" / "main.cpp",
                   "#include <cstdio>\n#include <clib/clib.h>\n#include \"helper.h\"\n"
                   "int main() { std::printf(\"%d\\n\", helper(clib_twice(20))); }\n");

    build::Project project = build::load_project(app, "debug", std::nullopt, {});
    install::install(project, {.cwd = app, .add = {"../clib"}}, {});
    auto lock = lock::load(app, app);
    CHECK((lock && lock->find("clib") && lock->find("clib")->std == "c++11"));

    build::BuildResult built = build_with_tests(app);
    CHECK_EQ(built.exit_code, 0);
    CHECK_EQ(run_program(app / built.plan.main_output), "41");
}

TEST(a_c_library_from_a_registry_builds_in_the_sandbox) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    manifest::UserConfig config;
    config.registries["default"] = {reg.url(), false};
    set_env("CPM_HOME", t.path() / "home");

    reg.publish("csum", "1.0.0",
                {{"cpm.jsonc", R"({ "name": "csum", "version": "1.0.0", "cstd": "c99" })"},
                 {"include/csum.h", "#ifndef CSUM_H\n#define CSUM_H\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                                    "int csum(const int* v, int n);\n#ifdef __cplusplus\n}\n#endif\n#endif\n"},
                 {"src/lib.c", "#if __STDC_VERSION__ != 199901L\n#error not C99\n#endif\n#include <csum.h>\n"
                               "int csum(const int* v, int n) { int s = 0; for (int i = 0; i < n; ++i) s += v[i]; return s; }\n"}},
                fixture::entry("static-lib", "c++11"));

    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++17" })");
    fixture::write(app / "src" / "main.cpp",
                   "#include <cstdio>\n#include <csum.h>\nint main() { int v[] = {1, 2, 3}; std::printf(\"%d\\n\", csum(v, 3)); }\n");

    build::Project project = build::load_project(app, "debug", std::nullopt, config);
    install::install(project, {.cwd = app, .add = {"csum"}}, config);
    CHECK(fs::exists(t.path() / "home" / "store" / "csum@1.0.0" / "lib" / project.abi.dir() / ".complete"));
    build::BuildResult built = build_with_tests(app, config);
    CHECK_EQ(built.exit_code, 0);
    CHECK_EQ(run_program(app / built.plan.main_output), "6");
}

TEST(zlib_builds_from_git_with_an_overlay) {
    // Needs github.com, so it runs only where CPM_TEST_NETWORK is set (CI sets it).
    if (!env::is_set("CPM_TEST_NETWORK") || !cpm::proc::find_program("git")) {
        std::puts("CPM_TEST_NETWORK is not set or git is missing; skipping");
        return;
    }
    check::TempDir t;
    set_env("CPM_HOME", t.path() / "home");
    fs::path app = t.path() / "app";
    // zlib has no cpm.jsonc. Its sources are the .c files at the top; HAVE_UNISTD_H is what ./configure sets on Unix.
    fixture::write(app / "cpm.jsonc", R"({
  "name": "app", "version": "1.0.0", "std": "c++17",
  "dependencies": {
    "zlib": {
      "git": "https://github.com/madler/zlib.git", "tag": "v1.3.1",
      "manifest": {
        "type": "static-lib", "cstd": "gnu99", "sources": ["*.c"], "include": ".",
        "platforms": { "unix": { "publicDefines": ["HAVE_UNISTD_H"] } }
      }
    }
  }
})");
    fixture::write(app / "src" / "main.cpp", R"(#include <cstdio>
#include <cstring>
#include <zlib.h>
int main() {
    const char text[] = "CPM builds zlib from a cpm.jsonc overlay. CPM builds zlib from a cpm.jsonc overlay.";
    unsigned char packed[256], unpacked[256];
    uLongf packed_size = sizeof packed, unpacked_size = sizeof unpacked;
    if (compress(packed, &packed_size, reinterpret_cast<const Bytef*>(text), sizeof text) != Z_OK) return 1;
    if (uncompress(unpacked, &unpacked_size, packed, packed_size) != Z_OK) return 2;
    if (unpacked_size != sizeof text || std::memcmp(unpacked, text, sizeof text) != 0) return 3;
    std::printf("zlib %s\n", zlibVersion());
}
)");
    build::Project project = build::load_project(app, "debug", std::nullopt, {});
    install::install(project, {.cwd = app}, {});
    build::BuildResult built = build_with_tests(app);
    CHECK_EQ(built.exit_code, 0);
    CHECK_EQ(run_program(app / built.plan.main_output), "zlib 1.3.1");
}
