// cpm outdated: Current from cpm-lock.json, Wanted and Latest from file:// registries (docs/spec/cli.md).

#include "build/pipeline.hpp"
#include "install/installer.hpp"
#include "install/outdated.hpp"
#include "install/registry.hpp"
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

const std::string kHeader = "#pragma once\n";

void release(const fixture::Registry& reg, const std::string& name, const std::string& version, bool yanked = false) {
    std::string manifest = std::format(R"({{ "name": "{}", "version": "{}", "type": "header-only" }})", name, version);
    Value e = entry("header-only");
    if (yanked) e.set("yanked", true);
    reg.publish(name, version, {{"cpm.jsonc", manifest}, {"include/x.hpp", kHeader}}, e);
}

std::string text(const std::optional<semver::Version>& v) { return v ? v->to_string() : "-"; }

lock::RootDependency root_dep(const std::string& specifier, const std::string& version) {
    return {specifier, *semver::Version::parse(version)};
}

}  // namespace

TEST(newest_prefers_releases_and_skips_yanked_versions) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    for (const char* v : {"1.0.0", "1.1.0", "2.0.0", "2.1.0-beta.1"}) release(reg, "a", v);
    release(reg, "a", "2.2.0", true);
    release(reg, "pre", "0.1.0-alpha.1");
    release(reg, "pre", "0.1.0-alpha.2");
    install::Registries regs(config_for(reg), t.path() / "home", install::Freshness::Online);
    const install::RegistryPackage& a = *regs.get("").package("a");
    auto range = [](const char* r) { return *semver::Range::parse(r); };

    CHECK_EQ(text(install::newest(a, nullptr)), "2.0.0");  // not the yanked 2.2.0, nor the prerelease
    auto caret1 = range("^1");
    CHECK_EQ(text(install::newest(a, &caret1)), "1.1.0");
    auto beta = range("^2.1.0-beta.0");
    CHECK_EQ(text(install::newest(a, &beta)), "2.1.0-beta.1");  // a range that opts into the prerelease
    auto none = range("^3");
    CHECK(!install::newest(a, &none).has_value());
    CHECK_EQ(text(install::newest(*regs.get("").package("pre"), nullptr)), "0.1.0-alpha.2");  // no release at all
}

TEST(outdated_compares_the_lock_with_the_range_and_the_registry) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    for (const char* v : {"1.0.0", "1.1.0", "2.0.0"}) release(reg, "a", v);
    release(reg, "b", "1.0.0");
    release(reg, "c", "1.0.0");
    release(reg, "c", "1.1.0", true);
    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0",
        "dependencies": { "a": "^1", "lib": "file:../lib", "b": "~1.0" },
        "devDependencies": { "g": "git+https://example.invalid/g.git#v1", "c": "^1" } })");
    manifest::Manifest m = manifest::load(app / "cpm.jsonc", app);
    install::Registries regs(config_for(reg), t.path() / "home", install::Freshness::Online);

    lock::Lockfile lock;
    lock.dependencies["a"] = root_dep("^1", "1.0.0");
    lock.dependencies["b"] = root_dep("~1.0", "1.0.0");
    lock.dev_dependencies["c"] = root_dep("^1", "1.0.0");
    install::OutdatedReport r = install::outdated(m, &lock, regs);

    CHECK_EQ(r.skipped.size(), 2u);  // path and git dependencies are not compared
    if (r.skipped.size() == 2) {
        CHECK_EQ(r.skipped[0]->name, "lib");
        CHECK_EQ(r.skipped[1]->name, "g");
    }
    CHECK_EQ(r.entries.size(), 3u);
    if (r.entries.size() != 3) return;
    const auto& a = r.entries[0];
    CHECK_EQ(a.name, "a");
    CHECK_EQ(text(a.current), "1.0.0");
    CHECK_EQ(text(a.wanted), "1.1.0");
    CHECK_EQ(text(a.latest), "2.0.0");
    CHECK(a.outdated());
    CHECK(!a.dev);
    const auto& b = r.entries[1];
    CHECK_EQ(b.name, "b");
    CHECK(!b.outdated());  // 1.0.0 everywhere
    const auto& c = r.entries[2];
    CHECK_EQ(c.name, "c");
    CHECK(c.dev);
    CHECK_EQ(text(c.latest), "1.0.0");  // 1.1.0 is yanked
    CHECK(!c.outdated());

    // A locked version that was yanked since is behind nothing, but no longer what a resolution wants.
    lock.dev_dependencies["c"] = root_dep("^1", "1.1.0");
    CHECK(install::outdated(m, &lock, regs).entries[2].outdated());

    // Without a lockfile nothing is current.
    install::OutdatedReport fresh = install::outdated(m, nullptr, regs);
    CHECK(!fresh.entries[1].current.has_value());
    CHECK(fresh.entries[1].outdated());

    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "dependencies": { "zzz": "^1" } })");
    CHECK_THROWS(install::outdated(manifest::load(app / "cpm.jsonc", app), &lock, regs), "has no package \"zzz\"");
}

TEST(outdated_follows_installs_and_updates) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    auto config = config_for(reg);
    release(reg, "a", "1.0.0");
    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "dependencies": { "a": "^1" } })");
    set_env("CPM_HOME", t.path() / "home");
    build::Project project = build::load_project(app, "debug", std::nullopt, config);
    auto check_a = [&](const char* current, const char* wanted, const char* latest) {
        install::Registries regs(config, t.path() / "home", install::Freshness::Online);
        auto lock = lock::load(app, app);
        auto r = install::outdated(project.manifest, lock ? &*lock : nullptr, regs);
        CHECK_EQ(r.entries.size(), 1u);
        if (r.entries.size() != 1) return;
        CHECK_EQ(text(r.entries[0].current), current);
        CHECK_EQ(text(r.entries[0].wanted), wanted);
        CHECK_EQ(text(r.entries[0].latest), latest);
    };

    check_a("-", "1.0.0", "1.0.0");
    install::install(project, {.cwd = app}, config);
    check_a("1.0.0", "1.0.0", "1.0.0");
    release(reg, "a", "1.1.0");
    release(reg, "a", "2.0.0");
    check_a("1.0.0", "1.1.0", "2.0.0");
    install::install(project, {.cwd = app, .update_all = true}, config);
    check_a("1.1.0", "1.1.0", "2.0.0");  // update stays within the range
}
