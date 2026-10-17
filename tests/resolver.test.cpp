#include <fstream>

#include "install/default_registry.hpp"
#include "install/resolver.hpp"
#include "install/sources.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A path package with the given manifest body and an include/ directory.
void package(const fs::path& dir, const std::string& name, const std::string& version, const std::string& extra = "") {
    write(dir / "cpm.jsonc", "{ \"name\": \"" + name + "\", \"version\": \"" + version + "\", \"std\": \"c++17\"" + extra + " }");
    write(dir / "include" / name / (name + ".hpp"), "#pragma once\n");
}

manifest::Manifest root_manifest(const fs::path& dir, const std::string& deps, const std::string& dev = "{}") {
    std::string text = "{ \"name\": \"app\", \"version\": \"1.0.0\", \"std\": \"c++20\", \"dependencies\": " + deps +
                       ", \"devDependencies\": " + dev + " }";
    write(dir / "cpm.jsonc", text);
    return manifest::load(dir / "cpm.jsonc", dir);
}

install::Resolution resolve(const fs::path& root, const manifest::Manifest& m) {
    manifest::UserConfig no_registries;
    install::Registries registries(no_registries, root / "home", install::Freshness::Offline);
    install::Sources sources(root, root / "home", registries, true);
    return install::resolve(m, sources, nullptr);
}

}  // namespace

TEST(resolves_transitive_path_packages_in_dependency_order) {
    check::TempDir t;
    fs::path w = t.path();
    package(w / "b", "b", "1.2.0");
    package(w / "a", "a", "0.1.0", R"(, "dependencies": { "b": "file:../b" })");
    package(w / "c", "c", "3.0.0", R"(, "dependencies": { "b": "^1.0" })");  // a range on a path package
    package(w / "t", "t", "1.0.0");
    auto m = root_manifest(w / "app", R"({ "a": "file:../a", "b": "file:../b", "c": "file:../c" })", R"({ "t": "file:../t" })");
    auto res = resolve(w / "app", m);
    CHECK_EQ(res.packages.size(), 4u);
    CHECK_EQ(res.packages.at("b").meta.version.to_string(), "1.2.0");
    CHECK(!res.packages.at("b").dev);
    CHECK(res.packages.at("t").dev);
    // Every package before what it depends on; ties by name.
    CHECK_EQ(res.order, (std::vector<std::string>{"a", "c", "b", "t"}));
    CHECK_EQ(res.packages.at("b").dependents.size(), 3u);
}

TEST(reports_unsatisfiable_ranges) {
    check::TempDir t;
    fs::path w = t.path();
    package(w / "b", "b", "1.2.0");
    package(w / "c", "c", "3.0.0", R"(, "dependencies": { "b": "^2.0.0" })");
    auto m = root_manifest(w / "app", R"({ "b": "file:../b", "c": "file:../c" })");
    CHECK_THROWS(resolve(w / "app", m), "no version of \"b\" satisfies every constraint");
    try {
        resolve(w / "app", m);
    } catch (const Error& e) {
        std::string what = e.what();
        CHECK(what.contains("b ^2.0.0   required by c@3.0.0"));
        CHECK(what.contains("available: 1.2.0"));
    }
}

TEST(rejects_conflicting_sources_unless_the_project_chooses) {
    check::TempDir t;
    fs::path w = t.path();
    package(w / "b1", "b", "1.0.0");
    package(w / "b2", "b", "1.0.0");
    package(w / "x", "x", "1.0.0", R"(, "dependencies": { "b": "file:../b1" })");
    package(w / "y", "y", "1.0.0", R"(, "dependencies": { "b": "file:../b2" })");
    auto conflict = root_manifest(w / "app", R"({ "x": "file:../x", "y": "file:../y" })");
    CHECK_THROWS(resolve(w / "app", conflict), "requested from different sources");
    auto chosen = root_manifest(w / "app", R"({ "x": "file:../x", "y": "file:../y", "b": "file:../b2" })");
    CHECK(resolve(w / "app", chosen).packages.at("b").meta.dir.filename() == "b2");
}

TEST(checks_names_types_and_standards) {
    check::TempDir t;
    fs::path w = t.path();
    package(w / "wrong", "other", "1.0.0");
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "lib": "file:../wrong" })")), "names the package \"other\"");

    write(w / "exe" / "cpm.jsonc", R"({ "name": "exe", "version": "1.0.0", "type": "executable" })");
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "exe": "file:../exe" })")), "is an executable");

    write(w / "new" / "cpm.jsonc", R"({ "name": "new", "version": "1.0.0", "std": "c++23" })");
    write(w / "new" / "include" / "new.hpp", "#pragma once\n");
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "new": "file:../new" })")), "requires c++23");

    // With no registry configured, a registry dependency goes to the built-in default (offline here, so it fails
    // without touching the network).
    const std::string not_cached = std::string(install::kBuiltinRegistry) + "config.json is not cached";
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "fmt": "^10" })")), not_cached.c_str());
}

TEST(detects_cycles) {
    check::TempDir t;
    fs::path w = t.path();
    package(w / "a", "a", "1.0.0", R"(, "dependencies": { "b": "file:../b" })");
    package(w / "b", "b", "1.0.0", R"(, "dependencies": { "a": "file:../a" })");
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "a": "file:../a" })")), "cycle");
}

TEST(overlays_supply_missing_manifests) {
    check::TempDir t;
    fs::path w = t.path();
    write(w / "raw" / "inc" / "raw.hpp", "#pragma once\n");  // no cpm.jsonc
    auto m = root_manifest(w / "app",
                           R"({ "raw": { "path": "../raw", "manifest": { "version": "2.1.0", "type": "header-only", "include": "inc" } } })");
    auto res = resolve(w / "app", m);
    CHECK_EQ(res.packages.at("raw").meta.version.to_string(), "2.1.0");
    CHECK_EQ(res.packages.at("raw").meta.manifest.include_dir, "inc");
    CHECK_THROWS(resolve(w / "app", root_manifest(w / "app", R"({ "raw": "file:../raw" })")), "has no cpm.jsonc");
}
