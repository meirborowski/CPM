// Exported symbols (ROADMAP.md 0.4.9, manifest.md §4.1): each library package's <NAME>_API macro, the definitions a
// build gives each kind of compile, and a shared library made by `cpm init --shared-lib` built and tested with every
// compiler family the machine has.

#include <algorithm>
#include <format>

#include "build/exports.hpp"
#include "build/layout.hpp"
#include "build/ninja_gen.hpp"
#include "build/ninja_writer.hpp"
#include "build/pipeline.hpp"
#include "build/plan.hpp"
#include "core/env.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "init/init.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"
#include "toolchain/driver.hpp"
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
using build::ExportUse;
using manifest::TargetKind;

namespace {

void set_env(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

toolchain::Toolchain fake(Os os) {
    toolchain::Toolchain tc;
    tc.family = toolchain::Family::Gcc;
    tc.cxx = "/usr/bin/g++";
    tc.ar = "/usr/bin/gcc-ar";
    tc.version = "13.2.0";
    tc.os = os;
    return tc;
}

bool has(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// The plan of a package of `kind` named "my-lib" with one dependency and one dev dependency.
build::BuildPlan plan_for(const fs::path& dir, TargetKind kind, Os os) {
    const char* type = kind == TargetKind::SharedLib ? "shared-lib" : kind == TargetKind::StaticLib ? "static-lib" : "executable";
    fixture::write(dir / "src" / (kind == TargetKind::Executable ? "main.cpp" : "lib.cpp"), "int f() { return 1; }\n");
    fixture::write(dir / "tests" / "f.test.cpp", "int main() {}\n");
    manifest::Manifest m = manifest::parse(std::format(R"({{ "name": "my-lib", "version": "1.0.0", "type": "{}" }})", type),
                                           dir / "cpm.jsonc", "cpm.jsonc");
    toolchain::Toolchain tc = fake(os);
    auto driver = toolchain::make_driver(tc);
    build::Layout layout = build::infer_layout(m, {});
    build::Dependency json;
    json.name = "cpm-json";
    json.version = "0.1.0";
    json.identity = "sha256:x";
    build::Dependency test = json;
    test.name = "cpm-test";
    test.dev = true;
    return build::make_plan(m, m.profiles.at("debug"), tc, {.cxx = "-std=c++20"}, *driver, layout, {json, test});
}

}  // namespace

TEST(export_macros_are_named_after_their_packages) {
    CHECK_EQ(build::export_macro("cpm-json"), "CPM_JSON_API");
    CHECK_EQ(build::export_macro("fmt"), "FMT_API");
    CHECK_EQ(build::export_macro("my.lib-2"), "MY_LIB_2_API");
}

TEST(export_macros_mark_both_sides_of_a_shared_library) {
    CHECK_EQ(build::export_definition("mylib", ExportUse::BuildingShared, Os::Windows), "MYLIB_API=__declspec(dllexport)");
    CHECK_EQ(build::export_definition("mylib", ExportUse::LinkingShared, Os::Windows), "MYLIB_API=__declspec(dllimport)");
    CHECK_EQ(build::export_definition("mylib", ExportUse::BuildingShared, Os::Linux),
             "MYLIB_API=__attribute__((visibility(\"default\")))");
    CHECK_EQ(build::export_definition("mylib", ExportUse::LinkingShared, Os::MacOS),
             "MYLIB_API=__attribute__((visibility(\"default\")))");
    CHECK_EQ(build::export_definition("mylib", ExportUse::Plain, Os::Windows), "MYLIB_API=");
}

TEST(a_shared_library_exports_from_its_sources_and_imports_in_its_tests) {
    check::TempDir dir;
    build::BuildPlan plan = plan_for(dir.path(), TargetKind::SharedLib, Os::Windows);
    CHECK(has(plan.defines, "-DCPM_JSON_API="));        // a dependency: empty everywhere
    CHECK(!has(plan.defines, "-DCPM_TEST_API="));       // a dev dependency: tests only
    CHECK(!has(plan.defines, "-DMY_LIB_API=__declspec(dllexport)"));  // not shared by every compile
    CHECK(plan.source_defines == std::vector<std::string>{"-DMY_LIB_API=__declspec(dllexport)"});
    CHECK(has(plan.test_defines, "-DMY_LIB_API=__declspec(dllimport)"));
    CHECK(has(plan.test_defines, "-DCPM_TEST_API="));

    // Quoted for the host's command interpreter: /bin/sh quotes the parentheses, cmd.exe's rules don't.
    std::string ninja = build::generate_ninja(plan, *toolchain::make_driver(fake(Os::Windows)));
    CHECK(ninja.contains("  defines = $defines " + build::ninja::args_value({"-DMY_LIB_API=__declspec(dllexport)"}) + "\n"));
    CHECK(ninja.contains(build::ninja::args_value({"-DMY_LIB_API=__declspec(dllimport)"})));
}

TEST(a_static_library_and_an_executable_define_their_macros_empty) {
    check::TempDir lib_dir;
    build::BuildPlan lib = plan_for(lib_dir.path(), TargetKind::StaticLib, Os::Windows);
    CHECK(lib.source_defines == std::vector<std::string>{"-DMY_LIB_API="});
    CHECK(has(lib.test_defines, "-DMY_LIB_API="));

    check::TempDir exe_dir;
    build::BuildPlan exe = plan_for(exe_dir.path(), TargetKind::Executable, Os::Linux);
    CHECK(exe.source_defines.empty());  // a program has no macro of its own
    CHECK(has(exe.defines, "-DCPM_JSON_API="));
}

TEST(a_path_dependency_compiles_with_empty_macros) {
    check::TempDir dir;
    fixture::write(dir.path() / "src" / "main.cpp", "int main() {}\n");
    manifest::Manifest m = manifest::parse(R"({ "name": "app", "version": "1.0.0" })", dir.path() / "cpm.jsonc", "cpm.jsonc");
    toolchain::Toolchain tc = fake(Os::Windows);
    auto driver = toolchain::make_driver(tc);
    build::Dependency dll;
    dll.name = "my-dll";
    dll.version = "0.1.0";
    dll.identity = "path:../my-dll";
    dll.sources = build::Dependency::Sources{.root = "../my-dll", .files = {"src/lib.cpp"}, .include_dirs = {"../my-dll/include"},
                                             .defines = {}, .cstd = "gnu17"};
    build::BuildPlan plan = build::make_plan(m, m.profiles.at("debug"), tc, {.cxx = "-std=c++20"}, *driver,
                                             build::infer_layout(m, {}), {dll});
    auto compile = std::find_if(plan.compiles.begin(), plan.compiles.end(),
                                [](const build::CompileStep& c) { return c.kind == build::CompileStep::Kind::Dependency; });
    CHECK(compile != plan.compiles.end());
    if (compile != plan.compiles.end()) CHECK(has(compile->defines, "-DMY_DLL_API="));  // built into an archive
}

TEST(a_shared_library_from_cpm_init_builds_and_tests_with_each_compiler_family) {
    check::TempDir t;
    set_env("CPM_HOME", t.path() / "home");
#ifdef __APPLE__
    const std::vector<const char*> wanted = {"apple-clang"};
#elif defined(_WIN32)
    const std::vector<const char*> wanted = {"gcc", "clang", "msvc"};
#else
    const std::vector<const char*> wanted = {"gcc", "clang"};
#endif
    std::vector<toolchain::Found> found = toolchain::list_compilers(t.path() / "home");
    std::vector<std::string> families;
    for (const char* family : wanted) {
        if (toolchain::best_match(found, *toolchain::parse_request(family))) families.push_back(family);
    }
    INFO("compiler families here: {}", str::join(families, ", "));
    CHECK(!families.empty());

    init::Options o;
    o.dir = t.path() / "greeter";
    o.name = "greeter";
    o.std = "c++17";
    o.kind = TargetKind::SharedLib;
    init::write(o);
    std::string header = fsx::try_read_file(o.dir / "include" / "greeter" / "greeter.hpp").value_or("");
    CHECK(header.contains("GREETER_API int add(int a, int b);"));

    for (const auto& family : families) {
        INFO("family {}", family);
        build::BuildRequest req;
        req.cwd = o.dir;
        req.toolchain = toolchain::Choice{family, "--toolchain"};
        req.targets = build::BuildRequest::Targets::MainAndTests;
        req.clean = true;
        build::BuildResult built = build::run_build(req, manifest::UserConfig{});
        CHECK_EQ(built.exit_code, 0);
        CHECK(!built.plan.tests.empty());
        if (built.plan.tests.empty()) continue;
        // The test links the library and finds it at run time: on Windows through PATH, as cpm test sets it.
        proc::Command test{.argv = {paths::to_utf8(o.dir / built.plan.tests[0].output)}, .cwd = o.dir};
#ifdef _WIN32
        std::string path = paths::to_native_utf8(o.dir / built.plan.out_dir);
        for (const auto& d : toolchain::runtime_dirs(built.plan.toolchain)) path += ";" + paths::to_native_utf8(d);
        test.env_set = {{"PATH", path + ";" + env::get("PATH").value_or("")}};
#endif
        CHECK_EQ(proc::capture(test).exit_code, 0);
    }
}
