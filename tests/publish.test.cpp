// cpm publish (docs/spec/registry.md §6): file selection, the checks before packing, deterministic archives,
// index documents, and publishing into a file:// registry that CPM then reads back.
#include "core/fsx.hpp"
#include "install/fetch.hpp"
#include "install/registry.hpp"
#include "manifest/config.hpp"
#include "publish/publish.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, std::string_view text) { fsx::write_file_atomic(p, text); }

// A static library package "greet" in `dir`, with `extra` spliced into its manifest.
manifest::Manifest make_package(const fs::path& dir, std::string_view extra = "", std::string_view version = "1.2.0") {
    write(dir / "cpm.jsonc", std::format(R"({{ "name": "greet", "version": "{}", "std": "c++17"{} }})", version, extra));
    write(dir / "include/greet/greet.hpp", "#pragma once\nint greet();\n");
    write(dir / "src/lib.cpp", "#include \"greet/greet.hpp\"\nint greet() { return 42; }\n");
    write(dir / "src/detail.hpp", "#pragma once\n");
    write(dir / "tests/greet.test.cpp", "int main() {}\n");
    write(dir / "LICENSE", "MIT\n");
    write(dir / "README.md", "# greet\n");
    write(dir / "notes.txt", "not published by default\n");
    write(dir / "cpm-lock.json", "{}\n");
    write(dir / "build/debug/libgreet.a", "binary");
    return manifest::load(dir / "cpm.jsonc", dir);
}

std::string file_url(const fs::path& dir) { return install::url::from_path(dir); }

}  // namespace

TEST(default_file_selection) {
    check::TempDir dir;
    auto m = make_package(dir.path());
    CHECK_EQ(publish::select_files(m), (std::vector<std::string>{"LICENSE", "README.md", "cpm.jsonc",
                                                                 "include/greet/greet.hpp", "src/detail.hpp", "src/lib.cpp"}));
}

TEST(files_replaces_the_defaults_but_never_drops_the_manifest_or_license) {
    check::TempDir dir;
    auto m = make_package(dir.path(), R"(, "files": ["include/**", "notes.txt", "build/**", "cpm-lock.json"])");
    CHECK_EQ(publish::select_files(m),
             (std::vector<std::string>{"LICENSE", "README.md", "cpm.jsonc", "include/greet/greet.hpp", "notes.txt"}));
}

TEST(pack_is_deterministic_and_refuses_what_cannot_be_published) {
    check::TempDir dir;
    auto m = make_package(dir.path());
    publish::Package a = publish::pack(m);
    publish::Package b = publish::pack(m);
    CHECK_EQ(a.archive, b.archive);
    CHECK_EQ(a.integrity, b.integrity);
    CHECK(a.integrity.starts_with("sha256:"));
    CHECK(a.type == manifest::TargetKind::StaticLib);
    CHECK_EQ(a.archive_path, "packages/greet/greet-1.2.0.tar.gz");

    check::TempDir priv;
    CHECK_THROWS(publish::pack(make_package(priv.path(), R"(, "private": true)")), "private");
    check::TempDir git;
    CHECK_THROWS(publish::pack(make_package(git.path(), R"(, "dependencies": { "x": "git+https://example.org/x.git#v1" })")),
                 "depends on \"x\" (git)");
    check::TempDir exe;
    write(exe.path() / "cpm.jsonc", R"({ "name": "tool", "version": "1.0.0" })");
    write(exe.path() / "src/main.cpp", "int main() {}\n");
    CHECK_THROWS(publish::pack(manifest::load(exe.path() / "cpm.jsonc", exe.path())), "only libraries can be published");
}

TEST(index_documents_gain_versions_in_order_and_never_change_one) {
    check::TempDir dir;
    auto m2 = make_package(dir.path(), R"(, "dependencies": { "fmt": "^11.0.0" })", "2.0.0");
    auto p2 = publish::pack(m2);
    std::string doc = publish::add_version(std::nullopt, m2, p2, "file:///r/");
    auto m1 = make_package(dir.path(), "", "1.0.0");
    auto p1 = publish::pack(m1);
    doc = publish::add_version(doc, m1, p1, "file:///r/");

    auto pkg = install::parse_index(doc, "greet", "file:///r/");
    CHECK_EQ(pkg.versions.size(), 2u);
    CHECK_EQ(pkg.versions[0].version.to_string(), "2.0.0");
    CHECK_EQ(pkg.versions[0].dependencies, (std::vector<std::pair<std::string, std::string>>{{"fmt", "^11.0.0"}}));
    CHECK_EQ(pkg.versions[0].source.url, "file:///r/packages/greet/greet-2.0.0.tar.gz");
    CHECK_EQ(pkg.versions[1].source.strip_components, 1);
    CHECK(doc.find("\"1.0.0\"") < doc.find("\"2.0.0\""));  // oldest first in the document
    CHECK_THROWS(publish::add_version(doc, m1, p1, "file:///r/"), "already published");
}

TEST(publishes_into_a_file_registry) {
    check::TempDir dir;
    fs::path registry = dir.path() / "registry";
    write(registry / "config.json", R"({ "apiVersion": 1, "name": "test" })");
    fs::path pkg_dir = dir.path() / "greet";
    make_package(pkg_dir);
    manifest::UserConfig config;
    publish::publish(pkg_dir, {.registry = file_url(registry)}, config, dir.path() / "home");

    auto doc = fsx::read_file(registry / "index/greet.json");
    auto pkg = install::parse_index(doc, "greet", file_url(registry) + "/");
    CHECK_EQ(pkg.versions.size(), 1u);
    fs::path archive = registry / "packages/greet/greet-1.2.0.tar.gz";
    CHECK_EQ("sha256:" + install::sha256_file(archive), pkg.versions[0].source.integrity);
    fs::path out = dir.path() / "extracted";
    install::extract_archive(archive, out, 1, "greet");
    CHECK_EQ(fsx::read_file(out / "src/lib.cpp"), fsx::read_file(pkg_dir / "src/lib.cpp"));
    CHECK(!fs::exists(out / "tests"));

    // --out writes the same two files elsewhere, for registries CPM cannot write to.
    fs::path upload = dir.path() / "upload";
    make_package(pkg_dir, "", "1.3.0");
    publish::publish(pkg_dir, {.registry = file_url(registry), .out = upload}, config, dir.path() / "home");
    CHECK(fs::exists(upload / "packages/greet/greet-1.3.0.tar.gz"));
    CHECK_EQ(install::parse_index(fsx::read_file(upload / "index/greet.json"), "greet", "file:///x/").versions.size(), 2u);
    CHECK_EQ(install::parse_index(fsx::read_file(registry / "index/greet.json"), "greet", "file:///x/").versions.size(), 1u);
}
