// MSVC (ROADMAP.md 0.4.8): finding Visual Studio and its toolsets, the environment vcvarsall.bat sets, probing
// cl.exe, the MSVC driver and fingerprint, and, where Visual Studio is installed, a real build with it.

#include <algorithm>

#include "build/pipeline.hpp"
#include "core/env.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "install/installer.hpp"
#include "manifest/profile.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"
#include "toolchain/abi.hpp"
#include "toolchain/driver.hpp"
#include "toolchain/msvc.hpp"
#include "toolchain/select.hpp"
#include "toolchain/visual_studio.hpp"

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
using toolchain::Family;

namespace {

toolchain::Toolchain fake_msvc() {
    toolchain::Toolchain tc;
    tc.family = Family::Msvc;
    tc.cxx = "C:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";
    tc.ar = "C:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/lib.exe";
    tc.linker = "C:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/link.exe";
    tc.version = "19.51.36260";
    tc.target = "x86_64-pc-windows-msvc";
    tc.os = Os::Windows;
    tc.arch = "x86_64";
    tc.stdlib = "msvc-stl";
    tc.stdlib_abi = "145";
    tc.crt = "ucrt";
    tc.deps_prefix = "Note: including file:";
    return tc;
}

manifest::Profile profile(const std::string& name) { return manifest::builtin_profiles().at(name); }

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

}  // namespace

TEST(vswhere_lists_installations) {
    auto list = toolchain::vs::parse_vswhere(R"([
      { "instanceId": "f770a691", "installationPath": "C:\\Program Files\\Microsoft Visual Studio\\18\\Community",
        "installationVersion": "18.11.12306.390", "displayName": "Visual Studio Community 2026",
        "catalog": { "productDisplayVersion": "18.11.0" } },
      { "installationPath": "C:\\BuildTools", "installationVersion": "17.14.36301.6" },
      { "displayName": "no path, skipped" }
    ])");
    CHECK_EQ(list.size(), 2u);
    CHECK_EQ(list[0].name, "Visual Studio Community 2026");
    CHECK_EQ(list[0].version, "18.11.0");
    CHECK_EQ(list[0].path, fs::path("C:\\Program Files\\Microsoft Visual Studio\\18\\Community"));
    CHECK_EQ(list[1].name, "Visual Studio");  // no display name
    CHECK_EQ(list[1].version, "17.14.36301.6");
    CHECK(toolchain::vs::parse_vswhere("[]").empty());
    CHECK_THROWS(toolchain::vs::parse_vswhere("{}"), "not a JSON array");
}

TEST(the_toolsets_of_an_installation_newest_first) {
    check::TempDir t;
    toolchain::VisualStudio vs{t.path() / "VS", "Visual Studio Test", "18.0.0", ""};
    const std::string arch = toolchain::vs::host_arch();
    for (const char* toolset : {"14.44.35207", "14.51.36231", "14.9.1"}) {
        fixture::write(vs.path / "VC" / "Tools" / "MSVC" / toolset / "bin" / ("Host" + arch) / arch / "cl.exe", "");
    }
    fs::create_directories(vs.path / "VC" / "Tools" / "MSVC" / "14.50.0" / "bin");  // a toolset without cl.exe
    auto dirs = toolchain::vs::compiler_dirs(vs);
    CHECK_EQ(dirs.size(), 3u);
    CHECK_EQ(dirs[0].parent_path().parent_path().parent_path().filename(), fs::path("14.51.36231"));
    CHECK_EQ(dirs[1].parent_path().parent_path().parent_path().filename(), fs::path("14.44.35207"));
    CHECK_EQ(dirs[2].parent_path().parent_path().parent_path().filename(), fs::path("14.9.1"));
}

TEST(cl_exe_knows_its_installation_and_toolset) {
    toolchain::VisualStudio known{"C:/Program Files/Microsoft Visual Studio/18/Community", "Visual Studio Community 2026",
                                  "18.11.0", ""};
    auto vs = toolchain::vs::locate(
        "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/HostX64/x64/cl.exe", {known});
    CHECK(vs.has_value());
    if (!vs) return;
    CHECK_EQ(vs->toolset, "14.51.36231");
    CHECK_EQ(vs->name, "Visual Studio Community 2026");
    CHECK_EQ(vs->version, "18.11.0");
    // Elsewhere it is a Visual Studio CPM wasn't told about, or no Visual Studio at all.
    auto other = toolchain::vs::locate("D:/BT/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe", {known});
    CHECK((other && other->name == "Visual Studio" && other->toolset == "14.44.35207"));
    CHECK(!toolchain::vs::locate("C:/tools/cl.exe", {known}).has_value());
}

TEST(vcvarsall_environment_keeps_what_it_added) {
    // `set` output: names in any case, PATH with vcvarsall.bat's directories in front of what was there.
    std::string set = "ALLUSERSPROFILE=C:\\ProgramData\r\n"
                      "INCLUDE=C:\\VS\\include;C:\\SDK\\ucrt\r\n"
                      "LIB=C:\\VS\\lib\\x64;C:\\SDK\\lib\r\n"
                      "LIBPATH=C:\\VS\\lib\\x64\r\n"
                      "Path=C:\\VS\\bin\\Hostx64\\x64;C:\\SDK\\bin;C:\\Windows\\system32;C:\\Tools\\;C:\\VS\\bin\\Hostx64\\x64\r\n"
                      "VSCMD_VER=18.0\r\n";
    auto env = toolchain::vs::parse_environment(set, "C:\\Windows\\System32;C:\\Tools");
    CHECK_EQ(env.at("INCLUDE"), "C:\\VS\\include;C:\\SDK\\ucrt");
    CHECK_EQ(env.at("LIB"), "C:\\VS\\lib\\x64;C:\\SDK\\lib");
    CHECK_EQ(env.at("LIBPATH"), "C:\\VS\\lib\\x64");
    CHECK_EQ(env.at("PATH"), "C:\\VS\\bin\\Hostx64\\x64;C:\\SDK\\bin");  // only what it added, once each
    CHECK(!env.contains("VSCMD_VER"));

    auto vars = toolchain::vs::process_variables(env, "C:\\Windows\\System32");
    auto value = [&](const std::string& name) {
        for (const auto& [k, v] : vars) {
            if (k == name) return v;
        }
        return std::string("<unset>");
    };
    CHECK_EQ(value("INCLUDE"), env.at("INCLUDE"));
    CHECK_EQ(value("PATH"), "C:\\VS\\bin\\Hostx64\\x64;C:\\SDK\\bin;C:\\Windows\\System32");
}

TEST(a_captured_environment_is_intact_while_its_directories_exist) {
    check::TempDir t;
    fs::create_directories(t.path() / "include");
    fs::create_directories(t.path() / "lib");
    std::map<std::string, std::string> env = {{"INCLUDE", paths::to_utf8(t.path() / "include")},
                                              {"LIB", paths::to_utf8(t.path() / "lib")}};
    CHECK(toolchain::vs::environment_intact(env));
    env["LIB"] += ";" + paths::to_utf8(t.path() / "uninstalled-sdk");
    CHECK(!toolchain::vs::environment_intact(env));
    CHECK(!toolchain::vs::environment_intact({{"INCLUDE", ""}}));  // no LIB at all
}

TEST(the_probe_reads_its_markers) {
    std::string out = "#line 1 \"probe.cpp\"\n"
                      "namespace std { enum : bool { __the_value = true }; }\n"
                      "cpm_probe_MSC_FULL_VER=195136260 \n"
                      "cpm_probe_MSVC_STL_VERSION=145\n"
                      "cpm_probe_M_X64 = 100\n"
                      "cpm_probe_M_ARM64=_M_ARM64\n";
    auto macros = toolchain::msvc::parse_probe(out);
    CHECK_EQ(macros.at("_MSC_FULL_VER"), "195136260");
    CHECK_EQ(macros.at("_MSVC_STL_VERSION"), "145");
    CHECK_EQ(macros.at("_M_X64"), "100");
    CHECK(!macros.contains("_M_ARM64"));  // not defined: it stayed its own name

    toolchain::Toolchain tc = toolchain::msvc::toolchain_from_probe(macros, "C:/vs/cl.exe");
    CHECK(tc.family == Family::Msvc);
    CHECK_EQ(tc.version, "19.51.36260");
    CHECK_EQ(tc.arch, "x86_64");
    CHECK_EQ(tc.target, "x86_64-pc-windows-msvc");
    CHECK_EQ(tc.stdlib, "msvc-stl");
    CHECK_EQ(tc.stdlib_abi, "145");
    CHECK_EQ(tc.crt, "ucrt");
    CHECK_THROWS(toolchain::msvc::toolchain_from_probe({}, "C:/x/cl.exe"), "does not look like MSVC");
}

TEST(msvc_versions_read_both_ways) {
    CHECK_EQ(toolchain::msvc::display_version("195136260"), "19.51.36260");
    CHECK_EQ(toolchain::msvc::display_version("190023026"), "19.0.23026");  // Visual Studio 2015
    CHECK_EQ(toolchain::msvc::full_version("19.51.36260"), "195136260");
    CHECK_EQ(toolchain::msvc::full_version("19.0.23026"), "190023026");
    CHECK(toolchain::msvc::is_cl("C:/vs/bin/CL.EXE"));
    CHECK(!toolchain::msvc::is_cl("C:/llvm/bin/clang-cl.exe"));
}

TEST(the_show_includes_prefix_is_found_in_any_language) {
    fs::path dir = "C:\\Users\\me\\.cpm\\tmp";
    CHECK_EQ(toolchain::msvc::parse_deps_prefix("msvc-deps.cpp\r\nNote: including file: C:\\Users\\me\\.cpm\\tmp\\msvc-deps.h\r\n", dir),
             std::optional<std::string>("Note: including file:"));
    CHECK_EQ(toolchain::msvc::parse_deps_prefix("Hinweis: Einlesen der Datei:  c:\\users\\me\\.cpm\\tmp\\msvc-deps.h\n", dir),
             std::optional<std::string>("Hinweis: Einlesen der Datei:"));
    CHECK(!toolchain::msvc::parse_deps_prefix("msvc-deps.cpp\n", dir).has_value());
}

TEST(c_standards_map_to_msvc_modes) {
    CHECK_EQ(toolchain::msvc::cstd_flag("c99"), "");  // MSVC's default C
    CHECK_EQ(toolchain::msvc::cstd_flag("gnu11"), "/std:c11");
    CHECK_EQ(toolchain::msvc::cstd_flag("c17"), "/std:c17");
    CHECK_EQ(toolchain::msvc::cstd_flag("c23"), "/std:clatest");
    CHECK_THROWS(toolchain::msvc::cstd_flag("c2y"), "no flag for the C standard");
}

TEST(msvc_requests_and_versions) {
    auto r = toolchain::parse_request("msvc >=19.44");
    CHECK((r && r->family == Family::Msvc));
    toolchain::Toolchain tc = fake_msvc();
    CHECK(toolchain::matches(tc, *r));
    CHECK(!toolchain::matches(tc, *toolchain::parse_request("msvc <19.50")));
    CHECK(!toolchain::older_than_supported(tc));
    tc.version = "19.39.33523";  // Visual Studio 2022 17.9
    CHECK(toolchain::older_than_supported(tc));
    CHECK_EQ(toolchain::minimum_version(Family::Msvc), "19.40");
}

TEST(the_msvc_driver_maps_profiles_to_flags) {
    toolchain::Toolchain tc = fake_msvc();
    auto driver = toolchain::make_driver(tc);
    auto debug = driver->profile_flags(profile("debug"), toolchain::Language::Cxx, "/std:c++20", false);
    for (const char* flag : {"/std:c++20", "/Od", "/Z7", "/W4", "/MD", "/utf-8", "/Zc:preprocessor", "/bigobj", "/EHsc",
                             "/Zc:__cplusplus", "/permissive-"}) {
        INFO("flag {}", flag);
        CHECK(contains(debug.compile, flag));
    }
    CHECK(contains(debug.link, "/DEBUG"));

    manifest::Profile release = profile("release");
    release.runtime = "static";
    release.debug_runtime = true;
    release.lto = true;
    release.warnings = "strict";
    release.werror = true;
    release.sanitizers = {"address", "undefined"};
    auto f = driver->profile_flags(release, toolchain::Language::C, "/std:c11", false);
    for (const char* flag : {"/std:c11", "/O2", "/MTd", "/GL", "/W4", "/w14365", "/w14265", "/WX", "/fsanitize=address"}) {
        INFO("flag {}", flag);
        CHECK(contains(f.compile, flag));
    }
    CHECK(!contains(f.compile, "/EHsc"));  // C++ only
    CHECK(!contains(f.compile, "/Z7"));
    CHECK(contains(f.link, "/LTCG"));
    CHECK_EQ(f.notes.size(), 1u);  // "undefined" dropped, with a note
    CHECK(driver->sanitizers(release) == std::vector<std::string>{"address"});

    // C89 and C99 have no flag: MSVC's default C.
    auto c99 = driver->profile_flags(profile("debug"), toolchain::Language::C, "", false);
    CHECK(std::none_of(c99.compile.begin(), c99.compile.end(), [](const std::string& s) { return s.empty() || s.starts_with("/std:"); }));

    manifest::Profile quiet = profile("debug");
    quiet.warnings = "none";
    quiet.opt = "s";
    auto q = driver->profile_flags(quiet, toolchain::Language::Cxx, "/std:c++20", false);
    CHECK(contains(q.compile, "/W0"));
    CHECK(contains(q.compile, "/O1"));
}

TEST(the_msvc_driver_names_files_and_rules) {
    auto driver = toolchain::make_driver(fake_msvc());
    CHECK_EQ(driver->object_ext(), ".obj");
    CHECK_EQ(driver->exe_file("app"), "app.exe");
    CHECK_EQ(driver->static_lib_file("cpm-test"), "cpm-test.lib");
    CHECK_EQ(driver->define_flag("NDEBUG"), "/DNDEBUG");
    CHECK_EQ(driver->include_flag("C:/x/include"), "/IC:/x/include");
    CHECK(driver->lib_flags("C:/store/lib", "fmt") == (std::vector<std::string>{"/LIBPATH:C:/store/lib", "fmt.lib"}));
    CHECK_EQ(driver->system_lib_flag("shell32"), "shell32.lib");
    CHECK_EQ(driver->system_lib_flag("ws2_32.lib"), "ws2_32.lib");
    CHECK_THROWS(driver->shared_link_flags("x.dll", "x.lib"), "shared libraries with MSVC come in CPM 0.4.9");

    auto rules = driver->rules();
    auto rule = [&](const std::string& name) {
        auto it = std::find_if(rules.begin(), rules.end(), [&](const toolchain::NinjaRule& r) { return r.name == name; });
        return it == rules.end() ? toolchain::NinjaRule{} : *it;
    };
    CHECK_EQ(rule("cxx").deps, "msvc");
    CHECK_EQ(rule("cxx").msvc_deps_prefix, "Note: including file:");
    CHECK(rule("cxx").command.contains("/showIncludes"));
    CHECK(rule("cc").command.starts_with("$cc "));
    CHECK(rule("ar").command.starts_with("$ar /nologo /OUT:$out"));
    CHECK(rule("link").command.contains("$linker"));
    CHECK(rule("link").command.contains("/SUBSYSTEM:CONSOLE"));  // so main() may come from a library
}

TEST(msvc_fingerprints_use_the_full_version_and_its_flags) {
    toolchain::Toolchain tc = fake_msvc();
    auto driver = toolchain::make_driver(tc);
    auto fp = toolchain::fingerprint(tc, *driver, profile("debug"), "c++23", {}, {});
    // abi.md §3: the MSVC example.
    CHECK_EQ(fp.canonical(),
             "abi.scheme=2\narch=x86_64\ncompiler=msvc\ncompiler.version=195136260\ncrt=ucrt\ndebug-runtime=false\n"
             "defines.abi=\nexceptions=on\nos=windows\npic=false\nrtti=on\nruntime=dynamic\nsanitizers=\nstd=c++23\n"
             "stdlib=msvc-stl\nstdlib.abi=145\ntarget=x86_64-pc-windows-msvc\n");
    auto off = toolchain::fingerprint(tc, *driver, profile("debug"), "c++23", {}, {"/EHs-c-", "/GR-"});
    CHECK(off.canonical().contains("exceptions=off\n"));
    CHECK(off.canonical().contains("rtti=off\n"));
    manifest::Profile debug_crt = profile("debug");
    debug_crt.debug_runtime = true;
    CHECK(toolchain::fingerprint(tc, *driver, debug_crt, "c++23", {}, {}).hash != fp.hash);
}

TEST(builds_tests_and_runs_with_visual_studio) {
#ifndef _WIN32
    std::puts("Visual Studio is Windows-only; skipping");
#else
    check::TempDir t;
    SetEnvironmentVariableW(L"CPM_HOME", (t.path() / "home").wstring().c_str());
    std::vector<toolchain::Found> found = toolchain::list_compilers(t.path() / "home");
    if (!toolchain::best_match(found, *toolchain::parse_request("msvc"))) {
        std::puts("no Visual Studio with the C++ tools here; skipping");
        return;
    }

    // A C library from a registry, built in the sandbox with MSVC.
    fixture::Registry reg(t.path() / "registry");
    manifest::UserConfig config;
    config.registries["default"] = {reg.url(), false};
    reg.publish("csum", "1.0.0",
                {{"cpm.jsonc", R"({ "name": "csum", "version": "1.0.0", "cstd": "c11" })"},
                 {"include/csum.h", "#ifndef CSUM_H\n#define CSUM_H\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                                    "int csum(const int* v, int n);\n#ifdef __cplusplus\n}\n#endif\n#endif\n"},
                 {"src/lib.c", "#if __STDC_VERSION__ != 201112L\n#error not C11\n#endif\n#include <csum.h>\n"
                               "int csum(const int* v, int n) { int s = 0; for (int i = 0; i < n; ++i) s += v[i]; return s; }\n"}},
                fixture::entry("static-lib", "c++11"));

    // A test harness whose main() is in a library, as cpm-test's is.
    fs::path harness = t.path() / "harness";
    fixture::write(harness / "cpm.jsonc", R"({ "name": "harness", "version": "0.1.0", "type": "static-lib" })");
    fixture::write(harness / "include" / "harness.hpp", "#pragma once\nint run_tests();\n");
    fixture::write(harness / "src" / "main.cpp", "#include <harness.hpp>\nint main() { return run_tests(); }\n");

    // A C++20 program with a C file of its own, built with strict warnings as errors.
    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "cstd": "c17",
        "devDependencies": { "harness": "file:../harness" },
        "profiles": { "strict": { "inherits": "debug", "warnings": "strict", "werror": true } } })");
    fixture::write(app / "src" / "twice.h", "#pragma once\n#ifdef __cplusplus\nextern \"C\"\n#endif\nint twice(int x);\n");
    fixture::write(app / "src" / "twice.c", "#if __STDC_VERSION__ != 201710L\n#error not C17\n#endif\n"
                                            "#include \"twice.h\"\nint twice(int x) { return 2 * x; }\n");
    fixture::write(app / "src" / "main.cpp",
                   "#include <cstdio>\n#include <csum.h>\n#include \"twice.h\"\n"
                   "#ifndef _MSC_VER\n#error not MSVC\n#endif\n"
                   "int main() { int v[] = {1, 2, 3}; std::printf(\"%d\\n\", twice(csum(v, 3))); }\n");
    fixture::write(app / "tests" / "twice.test.cpp", "#include <harness.hpp>\n#include \"twice.h\"\n"
                                                     "int run_tests() { return twice(21) == 42 ? 0 : 1; }\n");

    toolchain::Choice msvc{"msvc", "--toolchain"};
    build::Project project = build::load_project(app, "strict", msvc, config);
    CHECK(project.toolchain.family == Family::Msvc);
    CHECK(project.abi.canonical().contains("compiler=msvc\n"));
    install::install(project, {.cwd = app, .add = {"csum"}}, config);
    CHECK(fs::exists(t.path() / "home" / "store" / "csum@1.0.0" / "lib" / project.abi.dir() / "csum.lib"));

    build::BuildRequest req;
    req.cwd = app;
    req.profile = "strict";
    req.toolchain = msvc;
    req.targets = build::BuildRequest::Targets::MainAndTests;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    build::BuildResult built = build::run_build(req, config);
    CHECK_EQ(built.exit_code, 0);
    CHECK_EQ(built.plan.toolchain.chosen_by, "from --toolchain");
    auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
    CHECK(out.out.starts_with("12"));
    std::string ninja = fsx::try_read_file(app / "cpm_modules" / ".cache" / "strict" / "build.ninja").value_or("");
    CHECK(ninja.contains("deps = msvc"));
    CHECK(ninja.contains("msvc_deps_prefix = "));
    CHECK(!built.plan.tests.empty());
    if (built.plan.tests.empty()) return;
    auto test = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.tests[0].output)}});
    CHECK_EQ(test.exit_code, 0);
#endif
}
