// Registries: index parsing, downloads with integrity, multi-version resolution, and installs from a
// file:// registry including prebuilt binaries.

#include "build/pipeline.hpp"
#include "core/env.hpp"
#include "install/fetch.hpp"
#include "install/installer.hpp"
#include "install/registry.hpp"
#include "install/resolver.hpp"
#include "install/sources.hpp"
#include "install/store.hpp"
#include "lock/lockfile.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"

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
using fixture::entry;
using json::Value;

namespace {

void set_env(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

manifest::UserConfig config_for(const fixture::Registry& r) {
    manifest::UserConfig c;
    c.registries["default"] = {r.url(), false};
    return c;
}

manifest::Manifest app_manifest(const fs::path& dir, const std::string& deps) {
    fixture::write(dir / "cpm.jsonc", "{ \"name\": \"app\", \"version\": \"1.0.0\", \"std\": \"c++20\", \"dependencies\": " + deps + " }\n");
    return manifest::load(dir / "cpm.jsonc", dir);
}

std::map<std::string, std::string> resolve_versions(const fs::path& home, const manifest::UserConfig& config,
                                                   const manifest::Manifest& m, const lock::Lockfile* lock = nullptr) {
    install::Registries regs(config, home, install::Freshness::Online);
    install::Sources sources(m.root, home, regs, false);
    auto res = install::resolve(m, sources, lock);
    std::map<std::string, std::string> out;
    for (const auto& [name, p] : res.packages) out[name] = p.meta.version.to_string();
    return out;
}

const std::string kHeader = "#pragma once\n";

}  // namespace

TEST(url_helpers) {
    CHECK_EQ(install::url::as_base("https://r.example/cpm"), "https://r.example/cpm/");
    CHECK_EQ(install::url::join("https://r.example/cpm", "packages/a.tar.gz"), "https://r.example/cpm/packages/a.tar.gz");
    CHECK_EQ(install::url::join("https://r.example/cpm/", "/x/a.tar.gz"), "https://r.example/x/a.tar.gz");
    CHECK_EQ(install::url::join("https://r.example/cpm/", "https://cdn.example/a"), "https://cdn.example/a");
#ifdef _WIN32
    CHECK(install::url::to_path("file:///C:/reg/x%20y") == fs::path("C:/reg/x y"));
    CHECK_EQ(install::url::from_path("C:/reg"), "file:///C:/reg");
#else
    CHECK(install::url::to_path("file:///srv/reg") == fs::path("/srv/reg"));
#endif
    CHECK(install::valid_integrity("sha256:" + std::string(64, 'a')));
    CHECK(!install::valid_integrity("sha256:" + std::string(64, 'A')));
    CHECK(!install::valid_integrity("sha512:abc"));
}

TEST(validates_index_documents) {
    std::string good = R"({ "name": "fmt", "versions": { "1.0.0": {
        "source": { "url": "packages/fmt.tar.gz", "integrity": "sha256:)" + std::string(64, '0') + R"(" },
        "type": "static-lib", "std": "c++17", "dependencies": { "x": "^1" }, "yanked": true } } })";
    auto pkg = install::parse_index(good, "fmt", "file:///reg/");
    CHECK_EQ(pkg.versions.size(), 1u);
    CHECK_EQ(pkg.versions[0].source.url, "file:///reg/packages/fmt.tar.gz");
    CHECK(pkg.versions[0].yanked);
    CHECK_THROWS(install::parse_index(good, "other", "file:///reg/"), "does not match");
    std::string bad_range = good;
    bad_range.replace(bad_range.find("\"^1\""), 4, "\"~~\"");
    CHECK_THROWS(install::parse_index(bad_range, "fmt", "file:///reg/"), "invalid range");
    std::string bad_type = good;
    bad_type.replace(bad_type.find("static-lib"), 10, "executable");
    CHECK_THROWS(install::parse_index(bad_type, "fmt", "file:///reg/"), "invalid \"type\"");
    std::string bad_hash = good;
    bad_hash.replace(bad_hash.find("sha256:"), 7, "md5:");
    CHECK_THROWS(install::parse_index(bad_hash, "fmt", "file:///reg/"), "integrity");
}

TEST(downloads_are_verified_and_cached) {
    check::TempDir t;
    fixture::write(t.path() / "src" / "a.txt", "payload");
    fs::path archive = t.path() / "a.tar.gz";
    fixture::tar_gz(archive, t.path() / "src", "a.txt");
    std::string url = install::url::from_path(archive);
    std::string integrity = "sha256:" + install::sha256_file(archive);
    fs::path home = t.path() / "home";

    fs::path cached = install::cached_archive(home, url, integrity, false, false, "a");
    CHECK(fs::exists(cached));
    fs::remove(archive);  // the cache serves it from now on, even offline
    CHECK(install::cached_archive(home, url, integrity, false, true, "a") == cached);

    std::string wrong = "sha256:" + std::string(64, '1');
    fixture::tar_gz(archive, t.path() / "src", "a.txt");
    CHECK_THROWS(install::cached_archive(home, url, wrong, false, false, "a"), "integrity check failed");
    CHECK_THROWS(install::cached_archive(home, url, wrong, false, true, "b"), "--offline");
    CHECK_THROWS(install::download("http://example.invalid/x", t.path() / "x", false), "plain http");
}

TEST(resolves_registry_versions_with_backtracking) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    auto config = config_for(reg);
    for (const char* v : {"1.0.0", "1.1.0", "2.0.0", "1.2.0-beta.1"}) reg.publish("b", v, {{"include/b.hpp", kHeader}}, entry("header-only"));
    Value yanked = entry("header-only");
    yanked.set("yanked", true);
    reg.publish("b", "1.3.0", {{"include/b.hpp", kHeader}}, yanked);
    reg.publish("a", "1.5.0", {{"include/a.hpp", kHeader}}, entry("header-only", "c++17", {{"b", "^1"}}));
    reg.publish("a", "2.0.0", {{"include/a.hpp", kHeader}}, entry("header-only", "c++17", {{"b", "^2"}}));

    fs::path home = t.path() / "home";
    // Newest within range; yanked and prerelease versions are skipped.
    auto simple = resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "b": "^1.0" })"));
    CHECK_EQ(simple.at("b"), "1.1.0");
    // a@2.0.0 needs b ^2, which conflicts with the project's b ^1: back off to a@1.5.0.
    auto backtracked = resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "a": "*", "b": "^1" })"));
    CHECK_EQ(backtracked.at("a"), "1.5.0");
    CHECK_EQ(backtracked.at("b"), "1.1.0");
    // Opting into prereleases explicitly.
    auto pre = resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "b": "1.2.0-beta.1" })"));
    CHECK_EQ(pre.at("b"), "1.2.0-beta.1");
    // Nothing fits: the report names the constraint and what exists.
    CHECK_THROWS(resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "b": "^3" })")), "available: 2.0.0, 1.1.0, 1.0.0");
    CHECK_THROWS(resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "zzz": "^1" })")), "has no package \"zzz\"");

    // A locked version is kept while it still fits, even a yanked one.
    lock::Lockfile lock;
    lock::Package locked;
    locked.name = "b";
    locked.version = *semver::Version::parse("1.3.0");
    locked.resolution = lock::RegistryResolution{install::url::as_base(reg.url()), "", "sha256:" + std::string(64, '0')};
    lock.packages.emplace(locked.key(), locked);
    auto pinned = resolve_versions(home, config, app_manifest(t.path() / "app", R"({ "b": "^1.0" })"), &lock);
    CHECK_EQ(pinned.at("b"), "1.3.0");
}

TEST(update_drops_the_lock_preference_for_named_or_all_packages) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    auto config = config_for(reg);
    auto release = [&](const std::string& name, const std::string& version) {
        std::string manifest = std::format(R"({{ "name": "{}", "version": "{}", "type": "header-only" }})", name, version);
        reg.publish(name, version, {{"cpm.jsonc", manifest}, {"include/x.hpp", kHeader}}, entry("header-only"));
    };
    for (const char* name : {"a", "b"}) release(name, "1.0.0");
    fs::path app = t.path() / "app";
    app_manifest(app, R"({ "a": "^1", "b": "^1" })");
    set_env("CPM_HOME", t.path() / "home");
    build::Project project = build::load_project(app, "debug", std::nullopt, config);
    auto version_of = [&](const char* name) { return lock::load(app, app)->find(name)->version.to_string(); };

    install::install(project, {.cwd = app}, config);
    for (const char* name : {"a", "b"}) release(name, "1.1.0");
    install::install(project, {.cwd = app}, config);  // a plain install keeps what the lock has
    CHECK_EQ(version_of("a"), "1.0.0");
    install::install(project, {.cwd = app, .update = {"a"}}, config);
    CHECK_EQ(version_of("a"), "1.1.0");
    CHECK_EQ(version_of("b"), "1.0.0");
    install::install(project, {.cwd = app, .update_all = true}, config);
    CHECK_EQ(version_of("b"), "1.1.0");
    CHECK_THROWS(install::install(project, {.cwd = app, .update = {"zzz"}}, config), "\"zzz\" is not installed");
}

TEST(installs_from_a_registry_with_prebuilt_binaries) {
    check::TempDir t;
    const fs::path w = t.path();
    fixture::Registry reg(w / "registry");
    auto config = config_for(reg);

    // strutil has no cpm.jsonc: the index's manifest overlay describes it.
    Value strutil = entry("header-only", "c++17");
    Value overlay = Value::object();
    overlay.set("type", "header-only");
    overlay.set("include", "inc");
    overlay.set("publicDefines", Value(json::Array{Value("STRUTIL=1")}));
    strutil.set("manifest", overlay);
    reg.publish("strutil", "1.0.0", {{"inc/strutil/strutil.hpp", "#pragma once\n#include <string>\ninline std::string up(std::string s) { for (auto& c : s) c = char(c >= 'a' && c <= 'z' ? c - 32 : c); return s; }\n"}}, strutil);
    reg.publish("greet", "1.0.0",
                {{"cpm.jsonc", R"({ "name": "greet", "version": "1.0.0", "std": "c++17", "dependencies": { "strutil": "^1" } })"},
                 {"include/greet/greet.hpp", "#pragma once\n#include <string>\nstd::string greet(const std::string& who);\n"},
                 {"src/lib.cpp", "#include <greet/greet.hpp>\n#include <strutil/strutil.hpp>\n#ifndef STRUTIL\n#error overlay defines missing\n#endif\nstd::string greet(const std::string& w) { return up(\"hi \" + w); }\n"}},
                entry("static-lib", "c++17", {{"strutil", "^1"}}));

    fs::path app = w / "app";
    fixture::write(app / "cpm.jsonc", "{\n  \"name\": \"app\",\n  \"version\": \"1.0.0\",\n  \"std\": \"c++20\"\n}\n");
    fixture::write(app / "src" / "main.cpp", "#include <cstdio>\n#include <greet/greet.hpp>\nint main() { std::puts(greet(\"app\").c_str()); }\n");

    // First machine: builds greet from source.
    set_env("CPM_HOME", w / "home1");
    build::Project project = build::load_project(app, "debug", std::nullopt, config);
    install::install(project, {.cwd = app, .add = {"greet"}}, config);
    CHECK(fsx::read_file(app / "cpm.jsonc").contains("\"greet\": \"^1.0.0\""));
    auto lock = lock::load(app, app);
    CHECK(lock && std::holds_alternative<lock::RegistryResolution>(lock->find("greet")->resolution));
    CHECK(lock && lock->find("strutil")->type == manifest::TargetKind::HeaderOnly);
    fs::path lib_dir = w / "home1" / "store" / "greet@1.0.0" / "lib" / project.abi.dir();
    CHECK(fs::exists(lib_dir / ".complete"));

    build::BuildRequest req;
    req.cwd = app;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    auto built = build::run_build(req, config);
    CHECK_EQ(built.exit_code, 0);
    auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
    CHECK(out.out.starts_with("HI APP"));

    // Offline reinstall from the caches works.
    install::install(project, {.cwd = app, .offline = true}, config);

    // A store entry whose files were deleted from outside CPM is extracted again from the download cache.
    fs::path entry = w / "home1" / "store" / "greet@1.0.0";
    fsx::remove_tree(entry / "include");
    fsx::remove_tree(entry / "src");
    CHECK(!install::entry_intact(entry));
    CHECK_EQ(build::run_build(req, config).exit_code, 0);
    CHECK(install::entry_intact(entry));
    CHECK(install::library_intact(lib_dir));

    // Publish that build as a prebuilt binary; a second machine downloads it instead of compiling.
    reg.add_binary("greet", "1.0.0", lib_dir);
    set_env("CPM_HOME", w / "home2");
    build::Project fresh = build::load_project(app, "debug", std::nullopt, config);
    install::install(fresh, {.cwd = app}, config);
    CHECK(fs::exists(w / "home2" / "store" / "greet@1.0.0" / "lib" / fresh.abi.dir() / "libgreet.a"));
    CHECK(!fs::exists(w / "home2" / "logs"));  // no source build happened

    // A prebuilt binary for different ABI inputs is not used: the package is built from source instead.
    fs::path tampered = w / "tampered" / project.abi.dir();
    fs::create_directories(tampered);
    for (const auto& e : fs::directory_iterator(lib_dir)) fs::copy(e.path(), tampered / e.path().filename());
    std::string meta = fsx::read_file(tampered / "meta.json");
    meta.replace(meta.find("\"compiler.version\": \""), 21, "\"compiler.version\": \"9");
    fsx::write_file_atomic(tampered / "meta.json", meta);
    reg.add_binary("greet", "1.0.0", tampered);
    set_env("CPM_HOME", w / "home3");
    build::Project third = build::load_project(app, "debug", std::nullopt, config);
    install::install(third, {.cwd = app}, config);
    CHECK(fs::exists(w / "home3" / "logs"));  // built from source after rejecting the binary

    // --no-build leaves the library for the next build to make.
    set_env("CPM_HOME", w / "home4");
    build::Project fourth = build::load_project(app, "debug", std::nullopt, config);
    install::install(fourth, {.cwd = app, .no_build = true}, config);
    CHECK(!fs::exists(w / "home4" / "store" / "greet@1.0.0" / "lib" / fourth.abi.dir() / ".complete"));
    CHECK_EQ(build::run_build(req, config).exit_code, 0);
    CHECK(fs::exists(w / "home4" / "store" / "greet@1.0.0" / "lib" / fourth.abi.dir() / ".complete"));

    // Specs: -E saves the exact version, @range is kept as typed, --registry writes the object form.
    install::install(fourth, {.cwd = app, .add = {"strutil"}, .exact = true}, config);
    CHECK(fsx::read_file(app / "cpm.jsonc").contains("\"strutil\": \"1.0.0\""));
    install::install(fourth, {.cwd = app, .add = {"strutil@~1.0"}}, config);
    CHECK(fsx::read_file(app / "cpm.jsonc").contains("\"strutil\": \"~1.0\""));
    manifest::UserConfig named = config;
    named.registries["internal"] = {reg.url(), false};
    install::install(fourth, {.cwd = app, .add = {"strutil@^1"}, .registry = "internal"}, named);
    CHECK(fsx::read_file(app / "cpm.jsonc").contains("\"registry\": \"internal\""));

    // A registry that changes a published archive is caught by the recorded integrity.
    set_env("CPM_HOME", w / "home5");
    fs::path archive = w / "registry" / "packages" / "greet" / "greet-1.0.0.tar.gz";
    fixture::write(w / "evil" / "greet-1.0.0" / "cpm.jsonc", "{}");
    fixture::tar_gz(archive, w / "evil", "greet-1.0.0");
    build::Project fifth = build::load_project(app, "debug", std::nullopt, named);
    CHECK_THROWS(install::install(fifth, {.cwd = app}, named), "integrity check failed");
}

// Switching a dependency's version repoints cpm_modules links at older files under the same paths; the
// dependency stamp must still make everything that includes or links them rebuild.
TEST(switching_versions_rebuilds_dependents) {
    check::TempDir t;
    const fs::path w = t.path();
    fixture::Registry reg(w / "registry");
    auto config = config_for(reg);
    for (const char* v : {"1.0.0", "1.1.0"}) {
        reg.publish("hdr", v,
                    {{"cpm.jsonc", std::string(R"({ "name": "hdr", "version": ")") + v + R"(", "type": "header-only" })"},
                     {"include/hdr/hdr.hpp", std::string("#pragma once\ninline const char* hdr() { return \"") + v + "\"; }\n"}},
                    entry("header-only"));
    }
    fs::path app = w / "app";
    fixture::write(app / "cpm.jsonc", "{ \"name\": \"app\", \"version\": \"1.0.0\", \"std\": \"c++20\" }\n");
    fixture::write(app / "src" / "main.cpp", "#include <cstdio>\n#include <hdr/hdr.hpp>\nint main() { std::puts(hdr()); }\n");
    set_env("CPM_HOME", w / "home");
    build::Project project = build::load_project(app, "debug", std::nullopt, config);

    build::BuildRequest req;
    req.cwd = app;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    auto run_app = [&] {
        auto built = build::run_build(req, config);
        CHECK_EQ(built.exit_code, 0);
        auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
        return out.out.substr(0, out.out.find_first_of("\r\n"));
    };
    install::install(project, {.cwd = app, .add = {"hdr"}}, config);
    CHECK_EQ(run_app(), "1.1.0");
    install::install(project, {.cwd = app, .add = {"hdr@~1.0.0"}}, config);
    CHECK_EQ(run_app(), "1.0.0");
    install::install(project, {.cwd = app, .add = {"hdr@^1.1"}}, config);
    CHECK_EQ(run_app(), "1.1.0");
}
