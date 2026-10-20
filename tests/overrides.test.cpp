// Version overrides (docs/spec/dependencies.md §4.6): the manifest field, resolution, the lockfile, and what
// install, why, outdated and publish do with them.

#include "build/pipeline.hpp"
#include "core/fsx.hpp"
#include "install/inspect.hpp"
#include "install/installer.hpp"
#include "install/outdated.hpp"
#include "install/registry.hpp"
#include "install/resolver.hpp"
#include "install/sources.hpp"
#include "lock/lockfile.hpp"
#include "publish/publish.hpp"
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
using fixture::Index;

namespace {

void set_env(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.wstring().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

manifest::UserConfig config_for(const std::string& url) {
    manifest::UserConfig c;
    c.registries["default"] = {url, false};
    return c;
}

manifest::Manifest project(const fs::path& dir, const std::string& deps, const std::string& overrides = "{}") {
    fixture::write(dir / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "dependencies": )" + deps +
                                          R"(, "overrides": )" + overrides + " }\n");
    return manifest::load(dir / "cpm.jsonc", dir);
}

install::Resolution resolve(const Index& index, const fs::path& home, const manifest::Manifest& m) {
    install::Registries regs(config_for(index.url()), home, install::Freshness::Online);
    install::Sources sources(m.root, home, regs, false);
    return install::resolve(m, sources, nullptr);
}

std::string version_of(const install::Resolution& res, const std::string& name) {
    return res.packages.at(name).meta.version.to_string();
}

// fmt 9 and 10; spdlog 1.12.0 asks for fmt ^9.1.0 although it works with fmt 10.
Index fmt_and_spdlog(const fs::path& dir) {
    Index index(dir);
    index.add("fmt", "9.1.0").add("fmt", "10.2.1");
    index.add("spdlog", "1.12.0", {{"fmt", "^9.1.0"}});
    index.add("other", "1.0.0", {{"fmt", "^9.1.0"}});
    index.save();
    return index;
}

}  // namespace

TEST(manifests_declare_overrides) {
    auto parse = [](const std::string& overrides) {
        return manifest::parse(R"({ "name": "app", "version": "1.0.0", "overrides": )" + overrides + " }", "cpm.jsonc", "cpm.jsonc");
    };
    manifest::Manifest m = parse(R"({ "fmt": "^10.2.0", "spdlog>fmt": "~10.2" })");
    CHECK_EQ(m.overrides.size(), 2u);
    CHECK_EQ(m.overrides.at("spdlog>fmt"), "~10.2");
    for (const char* bad : {R"({ "Fmt": "^1" })", R"({ "fmt>fmt": "^1" })", R"({ "a>b>c": "^1" })", R"({ ">fmt": "^1" })",
                            R"({ "spdlog>": "^1" })"}) {
        CHECK_THROWS(parse(bad), "invalid override");
    }
    CHECK_THROWS(parse(R"({ "fmt": 10 })"), "a version range");
    CHECK_THROWS(parse(R"({ "fmt": "banana" })"), "invalid version range \"banana\"");
    CHECK_THROWS(parse(R"([ "fmt" ])"), "\"overrides\"");

    // The scoped form wins; requirements that name a git or path source have no range to replace.
    manifest::DependencySpec fmt;
    fmt.name = "fmt";
    fmt.range = "^9.1.0";
    CHECK_EQ(manifest::override_for(m, "spdlog", fmt)->second, "~10.2");
    CHECK_EQ(manifest::override_for(m, "other", fmt)->first, "fmt");
    fmt.kind = manifest::DependencySpec::Kind::Git;
    CHECK(manifest::override_for(m, "spdlog", fmt) == nullptr);
    fmt.name = "zlib";
    fmt.kind = manifest::DependencySpec::Kind::Registry;
    CHECK(manifest::override_for(m, "spdlog", fmt) == nullptr);
}

TEST(overrides_replace_a_dependency_s_range) {
    check::TempDir t;
    Index index = fmt_and_spdlog(t.path() / "registry");
    fs::path home = t.path() / "home", app = t.path() / "app";
    const std::string deps = R"({ "fmt": "^10.2.0", "spdlog": "^1.12.0" })";

    // Without an override, spdlog's fmt ^9.1.0 and the project's fmt ^10.2.0 can't both hold.
    CHECK_THROWS(resolve(index, home, project(app, deps)), "spdlog 1.12.0 depends on fmt ^9.1.0");

    for (const char* overrides : {R"({ "fmt": "^10.2.0" })", R"({ "spdlog>fmt": "^10.2.0" })"}) {
        install::Resolution res = resolve(index, home, project(app, deps, overrides));
        CHECK_EQ(version_of(res, "fmt"), "10.2.1");
        CHECK_EQ(version_of(res, "spdlog"), "1.12.0");
        CHECK_EQ(res.packages.at("spdlog").overridden, (std::map<std::string, std::string>{{"fmt", "^9.1.0"}}));
        CHECK(res.unused_overrides.empty());
    }

    // The scoped override wins over the plain one, and only for its dependent.
    auto res = resolve(index, home, project(app, R"({ "fmt": "^10.2.0", "spdlog": "^1.12.0", "other": "^1.0.0" })",
                                            R"({ "fmt": "^10.2.0", "other>fmt": "<=9.1.0 || >=10.2.0" })"));
    CHECK_EQ(version_of(res, "fmt"), "10.2.1");
    CHECK_EQ(res.packages.at("other").overridden.at("fmt"), "^9.1.0");

    // The project's own range still counts, and an override nobody's requirement uses is reported.
    CHECK_THROWS(resolve(index, home, project(app, R"({ "fmt": "^9.1.0", "spdlog": "^1.12.0" })", R"({ "fmt": "^10.2.0" })")),
                 "spdlog 1.12.0 depends on fmt ^10.2.0 (override)");
    res = resolve(index, home, project(app, R"({ "fmt": "^10.2.0" })", R"({ "fmt": "^10.2.0", "zlib": "^1" })"));
    CHECK_EQ(res.unused_overrides, (std::vector<std::string>{"fmt", "zlib"}));
}

TEST(explanations_name_the_override) {
    check::TempDir t;
    Index index = fmt_and_spdlog(t.path() / "registry");
    try {
        resolve(index, t.path() / "home", project(t.path() / "app", R"({ "spdlog": "^1.12.0" })", R"({ "fmt": "^11.0.0" })"));
        CHECK(false);
    } catch (const Error& e) {
        CHECK_EQ(std::string(e.what()),
                 "the project's dependencies can't be resolved\n"
                 "  Because the project depends on spdlog ^1.12.0 and spdlog 1.12.0 depends on fmt ^11.0.0 (override), which "
                 "matches no version (available: 10.2.1, 9.1.0), no versions satisfy every requirement.");
        CHECK_EQ(e.hint(), "change the range of spdlog, or the override \"fmt\", in cpm.jsonc; cpm info <package> shows what each "
                           "version depends on");
    }
}

TEST(dependencies_cannot_override) {
    check::TempDir t;
    Index index = fmt_and_spdlog(t.path() / "registry");
    // A path dependency's overrides would make this resolve, but only the project's count.
    fixture::write(t.path() / "lib" / "cpm.jsonc",
                   R"({ "name": "lib", "version": "1.0.0", "type": "header-only", "dependencies": { "spdlog": "^1.12.0" },
                        "overrides": { "fmt": "^10.2.0" } })");
    CHECK_THROWS(resolve(index, t.path() / "home", project(t.path() / "app", R"({ "fmt": "^10.2.0", "lib": "file:../lib" })")),
                 "the project's dependencies can't be resolved");

    // Nor can a published package carry them.
    fixture::write(t.path() / "pub" / "cpm.jsonc", R"({ "name": "pub", "version": "1.0.0", "overrides": { "fmt": "^10" } })");
    fixture::write(t.path() / "pub" / "include" / "pub.hpp", "#pragma once\n");
    CHECK_THROWS(publish::pack(manifest::load(t.path() / "pub" / "cpm.jsonc", t.path())), "cannot have");
}

TEST(the_lockfile_records_overrides) {
    lock::Lockfile lock;
    auto v = [](const char* text) { return *semver::Version::parse(text); };
    lock.dependencies["spdlog"] = {"^1.12.0", v("1.12.0")};
    lock.overrides = {{"fmt", "^10.2.0"}};
    lock::Package fmt;
    fmt.name = "fmt";
    fmt.version = v("10.2.1");
    fmt.resolution = lock::RegistryResolution{"https://r.example/", "https://r.example/fmt.tgz", "sha256:" + std::string(64, 'a')};
    lock::Package spdlog = fmt;
    spdlog.name = "spdlog";
    spdlog.version = v("1.12.0");
    spdlog.dependencies["fmt"] = v("10.2.1");
    spdlog.overridden["fmt"] = "^9.1.0";
    lock.packages.emplace(fmt.key(), fmt);
    lock.packages.emplace(spdlog.key(), spdlog);

    std::string text = lock::serialize(lock);
    CHECK(text.contains("      \"devDependencies\": {},\n      \"overrides\": {\n        \"fmt\": \"^10.2.0\"\n      }\n"));
    CHECK(text.contains("      \"overridden\": {\n        \"fmt\": \"^9.1.0\"\n      },\n      \"resolution\""));
    lock::Lockfile back = lock::parse(text, "cpm-lock.json");
    CHECK_EQ(lock::serialize(back), text);
    CHECK_EQ(back.find("spdlog")->overridden.at("fmt"), "^9.1.0");

    // Without overrides, neither key is written.
    lock.overrides.clear();
    lock.packages.at("spdlog@1.12.0").overridden.clear();
    CHECK(!lock::serialize(lock).contains("overrid"));

    // `cpm why` shows the override, then the dependent's own range.
    CHECK_EQ(install::overridden_requirement(back, *back.find("spdlog"), "fmt"), std::optional<std::string>("^10.2.0 (override of ^9.1.0)"));
    CHECK(!install::overridden_requirement(back, *back.find("fmt"), "spdlog"));
}

TEST(outdated_notes_overrides_that_are_no_longer_needed) {
    check::TempDir t;
    auto v = [](const char* text) { return *semver::Version::parse(text); };
    auto package = [&](const char* name, const char* version, std::map<std::string, std::string> overridden) {
        lock::Package p;
        p.name = name;
        p.version = v(version);
        p.resolution = lock::RegistryResolution{"https://r.example/", "https://r.example/x.tgz", "sha256:" + std::string(64, 'a')};
        p.overridden = std::move(overridden);
        return p;
    };
    lock::Lockfile lock;
    for (auto p : {package("fmt", "10.2.1", {}), package("spdlog", "1.12.0", {{"fmt", "^9.1.0"}}),
                   package("other", "2.0.0", {{"fmt", "^10.0.0"}})}) {
        lock.packages.emplace(p.key(), p);
    }
    auto m = project(t.path() / "app", "{}", R"({ "spdlog>fmt": "^10.2.0", "other>fmt": "^10.2.0", "zlib": "^1" })");
    CHECK_EQ(install::unneeded_overrides(m, lock),
             (std::vector<std::string>{"the override \"other>fmt\" is no longer needed: other accepts fmt 10.2.1 by itself",
                                       "the override \"zlib\" replaces no requirement; remove it from cpm.jsonc"}));
}

TEST(the_fmt_9_against_fmt_10_example_builds_with_an_override) {
    // ROADMAP.md 0.4.3: the example must fail without an override and build with one.
    check::TempDir t;
    const fs::path w = t.path();
    fixture::Registry reg(w / "registry");
    for (const char* version : {"9.1.0", "10.2.1"}) {
        reg.publish("fmt", version,
                    {{"cpm.jsonc", std::format(R"({{ "name": "fmt", "version": "{}", "type": "header-only" }})", version)},
                     {"include/fmt/core.h", std::format("#pragma once\n#define FMT_VERSION_TEXT \"{}\"\n", version)}},
                    entry("header-only"));
    }
    reg.publish("spdlog", "1.12.0",
                {{"cpm.jsonc", R"({ "name": "spdlog", "version": "1.12.0", "type": "header-only", "dependencies": { "fmt": "^9.1.0" } })"},
                 {"include/spdlog/spdlog.h", "#pragma once\n#include <fmt/core.h>\ninline const char* spdlog_fmt() { return FMT_VERSION_TEXT; }\n"}},
                entry("header-only", "c++17", {{"fmt", "^9.1.0"}}));
    auto config = config_for(reg.url());

    fs::path app = w / "app";
    auto manifest = [&](const std::string& overrides) {
        fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20",
            "dependencies": { "fmt": "^10.2.0", "spdlog": "^1.12.0" }, "overrides": )" + overrides + " }\n");
    };
    fixture::write(app / "src" / "main.cpp", "#include <cstdio>\n#include <spdlog/spdlog.h>\nint main() { std::puts(spdlog_fmt()); }\n");
    set_env("CPM_HOME", w / "home");

    manifest("{}");
    build::Project project = build::load_project(app, "debug", std::nullopt, config);
    CHECK_THROWS(install::install(project, {.cwd = app}, config), "spdlog 1.12.0 depends on fmt ^9.1.0");
    CHECK(!fs::exists(app / "cpm-lock.json"));

    manifest(R"({ "spdlog>fmt": "^10.2.0" })");
    project = build::load_project(app, "debug", std::nullopt, config);
    install::install(project, {.cwd = app}, config);
    auto lock = lock::load(app, app);
    CHECK(lock && lock->overrides.at("spdlog>fmt") == "^10.2.0");
    CHECK(lock && lock->find("spdlog")->overridden.at("fmt") == "^9.1.0");
    CHECK(lock && lock->find("fmt")->version.to_string() == "10.2.1");

    build::BuildRequest req;
    req.cwd = app;
    req.dependencies = [&](const build::Project& p) { return install::ensure(p, config); };
    auto built = build::run_build(req, config);
    CHECK_EQ(built.exit_code, 0);
    auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
    CHECK(out.out.starts_with("10.2.1"));

    // Changing the overrides makes the lockfile stale.
    manifest(R"({ "fmt": "^10.2.0" })");
    project = build::load_project(app, "debug", std::nullopt, config);
    CHECK_EQ(lock::staleness(*lock, project.manifest, [&](const lock::Package& p) {
        return install::path_package_manifest(app, project.manifest, p);
    }), std::optional<std::string>("the overrides changed"));
}
