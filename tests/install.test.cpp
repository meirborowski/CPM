// End to end: path and git dependencies through install, build, run, a frozen reinstall and remove.
// Uses a private $CPM_HOME and a local git repository (skipped when git is not installed).

#include <cstdlib>
#include <fstream>

#include "build/pipeline.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "install/git.hpp"
#include "install/installer.hpp"
#include "install/store.hpp"
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

// A static library in a local git repository, tagged v1.0.0.
fs::path make_greet_repo(const fs::path& dir) {
    write(dir / "cpm.jsonc", R"({ "name": "greet", "version": "1.0.0", "std": "c++17", "publicDefines": ["GREET=1"] })");
    write(dir / "include" / "greet" / "greet.hpp", "#pragma once\n#include <string>\nstd::string greet(const std::string& who);\n");
    write(dir / "src" / "lib.cpp", "#include <greet/greet.hpp>\nstd::string greet(const std::string& who) { return \"hi \" + who; }\n");
    git(dir, {"init", "-q"});
    git(dir, {"add", "-A"});
    git(dir, {"commit", "-qm", "v1"});
    git(dir, {"tag", "v1.0.0"});
    return dir;
}

// What a cleaner that deletes old files leaves of a store entry: CPM's own small files, which it wrote
// recently, and the libraries it built. Every file that came from the package is gone.
void delete_package_files(const fs::path& entry) {
    for (const auto& e : fsx::list_dir(entry)) {
        if (e.name == ".complete" || e.name == ".integrity" || e.name == ".source.json" || e.name == "lib") continue;
        fsx::remove_tree(entry / e.name);
    }
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

    // A git package.
    fs::path greet = make_greet_repo(w / "repos" / "greet");

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
    CHECK((g != nullptr && std::get<lock::GitResolution>(g->resolution).commit.size() == 40u));
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

// Something outside CPM (a cleaner of old files, say) deleted files under $CPM_HOME. The next build or
// install notices and makes the damaged store entry, library or git mirror again (docs/spec/store.md §2.3, §3).
TEST(damaged_store_entries_and_mirrors_are_made_again) {
    if (!cpm::proc::find_program("git")) {
        std::puts("git not found; skipping");
        return;
    }
    check::TempDir t;
    const fs::path w = t.path();
    const fs::path home = w / "home";
    set_env("CPM_HOME", home);

    fs::path greet = make_greet_repo(w / "repos" / "greet");
    fs::path app = w / "app";
    write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "dependencies": { "greet": "git+)" +
                                 file_url(greet) + "#v1.0.0\" } }");
    write(app / "src" / "main.cpp",
          "#include <cstdio>\n#include <greet/greet.hpp>\nint main() { std::printf(\"%s\\n\", greet(\"app\").c_str()); }\n");

    build::Project project = build::load_project(app, "debug", std::nullopt, {});
    install::install(project, {.cwd = app}, {});
    auto lock = lock::load(app, app);
    CHECK(lock.has_value());
    if (!lock) return;
    const lock::Package& g = *lock->find("greet");
    const fs::path entry = install::entry_dir(home, g);
    const fs::path lib = entry / "lib" / project.abi.dir();
    const std::string url = std::get<lock::GitResolution>(g.resolution).url;
    const fs::path mirror = install::git::mirror_dir(home, url);
    CHECK(install::entry_intact(entry));
    CHECK(install::library_intact(lib));
    CHECK(fsx::read_file(entry / ".complete").contains("include/greet/greet.hpp\n"));

    manifest::UserConfig config;
    build::BuildRequest req;
    req.cwd = app;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    auto build_and_run = [&] {
        build::BuildResult built = build::run_build(req, config);
        CHECK_EQ(built.exit_code, 0);
        auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
        CHECK_EQ(out.out.substr(0, out.out.find_first_of("\r\n")), "hi app");
    };

    // The package's files are gone but .complete is left: the build extracts the entry again.
    delete_package_files(entry);
    CHECK(!install::entry_intact(entry));
    build_and_run();
    CHECK(install::entry_intact(entry));
    CHECK(fs::exists(entry / "include" / "greet" / "greet.hpp"));

    // The same for an entry made before .complete listed files (an empty .complete), as in a home that an
    // earlier CPM filled.
    delete_package_files(entry);
    fsx::write_file_atomic(entry / ".complete", "");
    CHECK(!install::entry_intact(entry));
    build_and_run();
    CHECK(install::entry_intact(entry));
    CHECK(!fsx::read_file(entry / ".complete").empty());

    // The built library lost its archive but kept meta.json and .complete: it is built again.
    fs::remove(lib / project.driver->static_lib_file("greet"));
    CHECK(!install::library_intact(lib));
    build_and_run();
    CHECK(install::library_intact(lib));

    // The mirror is no longer a git repository (objects/ is gone), and the store entry must be made from it.
    fsx::remove_tree(mirror / "objects");
    fsx::remove_tree(entry);
    install::install(project, {.cwd = app}, {});
    CHECK(fs::exists(mirror / "objects"));
    CHECK(install::entry_intact(entry));

    // The mirror lost HEAD, so it does not look like a mirror at all; it is cloned again in its place.
    fs::remove(mirror / "HEAD");
    fsx::remove_tree(entry);
    install::install(project, {.cwd = app}, {});
    CHECK(fs::exists(mirror / "HEAD"));
    CHECK(install::entry_intact(entry));
    build_and_run();

    // A fetch that fails because the remote is unreachable keeps the mirror: only damage makes CPM clone again.
    fsx::write_file_atomic(mirror / "cpm-test-marker", "");
    fs::rename(greet, w / "repos" / "moved");
    write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "dependencies": { "greet": "git+)" +
                                 file_url(greet) + "#v2.0.0\" } }");
    build::Project changed = build::load_project(app, "debug", std::nullopt, {});
    CHECK_THROWS(install::install(changed, {.cwd = app}, {}), "cannot fetch");
    CHECK(fs::exists(mirror / "cpm-test-marker"));
}
