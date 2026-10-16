// End to end: path and git dependencies through install, build, run, a frozen reinstall and remove.
// Uses a private $CPM_HOME and a local git repository (skipped when git is not installed).

#include <cstdlib>
#include <fstream>

#include "build/pipeline.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "install/installer.hpp"
#include "lock/lockfile.hpp"
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

// cpm::proc is spelled out: macOS declares a global `struct proc` (<sys/proc.h>), so plain `proc::` is ambiguous.
using namespace cpm;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void git(const fs::path& dir, std::vector<std::string> args) {
    std::vector<std::string> argv = {"git", "-c", "user.name=cpm-test", "-c", "user.email=cpm@test", "-c",
                                     "init.defaultBranch=main", "-c", "core.autocrlf=false"};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = cpm::proc::capture({.argv = argv, .cwd = dir});
    if (r.exit_code != 0) throw std::runtime_error("git failed: " + r.err);
}

std::string file_url(const fs::path& dir) {
    std::string p = paths::to_utf8(fs::absolute(dir));
    return p.starts_with('/') ? "file://" + p : "file:///" + p;
}

}  // namespace

TEST(path_and_git_dependencies_end_to_end) {
    if (!cpm::proc::find_program("git")) {
        std::puts("git not found; skipping");
        return;
    }
    check::TempDir t;
    const fs::path w = t.path();
    set_env("CPM_HOME", w / "home");

    // A git package: a static library, tagged v1.0.0.
    fs::path greet = w / "repos" / "greet";
    write(greet / "cpm.jsonc", R"({ "name": "greet", "version": "1.0.0", "std": "c++17", "publicDefines": ["GREET=1"] })");
    write(greet / "include" / "greet" / "greet.hpp", "#pragma once\n#include <string>\nstd::string greet(const std::string& who);\n");
    write(greet / "src" / "lib.cpp", "#include <greet/greet.hpp>\nstd::string greet(const std::string& who) { return \"hi \" + who; }\n");
    git(greet, {"init", "-q"});
    git(greet, {"add", "-A"});
    git(greet, {"commit", "-qm", "v1"});
    git(greet, {"tag", "v1.0.0"});

    // A path package that uses it.
    fs::path mylib = w / "mylib";
    write(mylib / "cpm.jsonc", std::string(R"({ "name": "mylib", "version": "0.3.0", "std": "c++17", "dependencies": { "greet": ")") +
                                   "git+" + file_url(greet) + "#v1.0.0\" } }");
    write(mylib / "include" / "mylib" / "mylib.hpp", "#pragma once\n#include <string>\nstd::string banner();\n");
    write(mylib / "src" / "lib.cpp",
          "#include <mylib/mylib.hpp>\n#include <greet/greet.hpp>\n#ifndef GREET\n#error public define missing\n#endif\n"
          "std::string banner() { return \"[\" + greet(\"mylib\") + \"]\"; }\n");

    // The project.
    fs::path app = w / "app";
    write(app / "cpm.jsonc", "{\n  \"name\": \"app\",\n  \"version\": \"1.0.0\",\n  \"std\": \"c++20\",\n  // deps\n  \"dependencies\": {}\n}\n");
    write(app / "src" / "main.cpp",
          "#include <cstdio>\n#include <greet/greet.hpp>\n#include <mylib/mylib.hpp>\n"
          "int main() { std::printf(\"%s %s\\n\", greet(\"app\").c_str(), banner().c_str()); }\n");

    build::Project project = build::load_project(app, "debug", std::nullopt, {});
    install::install(project, {.cwd = app, .add = {"../mylib"}}, {});

    // cpm.jsonc gained the dependency and kept its comment; the lock pins the git commit.
    std::string manifest = fsx::read_file(app / "cpm.jsonc");
    CHECK(manifest.contains("// deps"));
    CHECK(manifest.contains("\"mylib\": \"file:../mylib\""));
    auto lock = lock::load(app, app);
    CHECK(lock.has_value());
    if (!lock) return;
    CHECK_EQ(lock->packages.size(), 2u);
    const lock::Package* g = lock->find("greet");
    CHECK(g != nullptr && std::get<lock::GitResolution>(g->resolution).commit.size() == 40u);
    CHECK(fsx::is_link(app / "cpm_modules" / "greet" / "include"));
    CHECK(fsx::is_link(app / "cpm_modules" / "greet" / "lib"));
    CHECK(fs::exists(app / "cpm_modules" / "mylib" / "include" / "mylib" / "mylib.hpp"));

    // Build and run with the installed dependencies.
    manifest::UserConfig config;
    build::BuildRequest req;
    req.cwd = app;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    build::BuildResult built = build::run_build(req, config);
    CHECK_EQ(built.exit_code, 0);
    auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
    CHECK_EQ(out.out.substr(0, out.out.find_first_of("\r\n")), "hi app [hi mylib]");

    // A frozen, offline reinstall changes nothing.
    std::string lock_text = fsx::read_file(app / "cpm-lock.json");
    install::install(project, {.cwd = app, .frozen = true, .offline = true}, {});
    CHECK_EQ(fsx::read_file(app / "cpm-lock.json"), lock_text);

    // The store keeps what was built; removing the only user unlinks it from cpm_modules.
    fs::path store = w / "home" / "store";
    std::size_t store_files = 0;
    for (auto& e : fs::recursive_directory_iterator(store)) store_files += e.is_regular_file();
    install::install(project, {.cwd = app, .remove = {"mylib"}}, {});
    CHECK(!fsx::read_file(app / "cpm.jsonc").contains("mylib"));
    CHECK(!fs::exists(app / "cpm_modules" / "greet"));
    CHECK(lock::load(app, app)->packages.empty());
    std::size_t after = 0;
    for (auto& e : fs::recursive_directory_iterator(store)) after += e.is_regular_file();
    CHECK_EQ(after, store_files);
}
