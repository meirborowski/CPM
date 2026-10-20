// cpm info, cpm tree and cpm why: what they print for a registry package and for a project's lockfile
// (docs/spec/cli.md).

#include <map>

#include "core/strings.hpp"
#include "install/inspect.hpp"
#include "install/registry.hpp"
#include "lock/lockfile.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"

using namespace cpm;
namespace fs = std::filesystem;
using fixture::entry;
using json::Value;

namespace {

semver::Version v(const char* text) { return *semver::Version::parse(text); }

manifest::UserConfig config_for(const fixture::Registry& r) {
    manifest::UserConfig c;
    c.registries["default"] = {r.url(), false};
    return c;
}

void release(const fixture::Registry& reg, const std::string& name, const std::string& version, Value e, bool yanked = false) {
    if (yanked) e.set("yanked", true);
    reg.publish(name, version, {{"include/x.hpp", "#pragma once\n"}}, std::move(e));
}

lock::Package package(const std::string& name, const char* version, lock::Resolution resolution,
                      std::map<std::string, semver::Version> deps = {}) {
    lock::Package p;
    p.name = name;
    p.version = v(version);
    p.resolution = std::move(resolution);
    p.dependencies = std::move(deps);
    return p;
}

lock::RegistryResolution from_registry(const std::string& name) {
    return {"https://cpm-registry.com/", "https://cpm-registry.com/packages/" + name + ".tar.gz", "sha256:" + std::string(64, 'a')};
}

// hello 0.1.0 depends on fmt, spdlog and the path package mylib; doctest, from git, is a devDependency. mylib
// depends on fmt and spdlog, and spdlog on fmt.
lock::Lockfile project_lock() {
    lock::Lockfile lock;
    lock.dependencies["fmt"] = {"^12.2.0", v("12.2.0")};
    lock.dependencies["spdlog"] = {"^1.17.0", v("1.17.0")};
    lock.dependencies["mylib"] = {"file:../mylib", v("0.1.0")};
    lock.dev_dependencies["doctest"] = {"git+https://github.com/doctest/doctest.git#v2.4.11", v("2.4.11")};
    for (auto p : {package("fmt", "12.2.0", from_registry("fmt")),
                   package("spdlog", "1.17.0", from_registry("spdlog"), {{"fmt", v("12.2.0")}}),
                   package("mylib", "0.1.0", lock::PathResolution{"../mylib"}, {{"fmt", v("12.2.0")}, {"spdlog", v("1.17.0")}}),
                   package("doctest", "2.4.11", lock::GitResolution{"https://github.com/doctest/doctest.git", "v2.4.11", std::string(40, 'b')})}) {
        if (p.name == "doctest") p.dev = true;
        lock.packages.emplace(p.key(), p);
    }
    return lock;
}

}  // namespace

TEST(info_shows_a_static_registrys_index) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    release(reg, "fmt", "12.0.0", entry("static-lib", "c++11"));
    release(reg, "fmt", "12.1.0", entry("static-lib", "c++11"), true);
    release(reg, "fmt", "12.2.0", entry("static-lib", "c++11"));
    release(reg, "fmt", "13.0.0-rc.1", entry("static-lib", "c++17"));
    install::Registries regs(config_for(reg), t.path() / "home", install::Freshness::Online);
    const install::RegistryPackage& fmt = *regs.get("").package("fmt");

    CHECK_EQ(install::render_info(fmt, *fmt.find(v("12.2.0")), std::nullopt, "cpm install fmt"),
             "fmt 12.2.0 · static library · C++11\n"
             "\n"
             "dependencies  none\n"
             "versions      13.0.0-rc.1, 12.2.0 (latest), 12.1.0 (yanked), 12.0.0\n"
             "\n"
             "install with  cpm install fmt\n");
}

TEST(info_adds_what_a_registry_service_knows) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    release(reg, "spdlog", "1.17.0", entry("static-lib", "c++11", {{"fmt", "^12.1.0"}, {"zlib", "^1.3"}}));
    install::Registries regs(config_for(reg), t.path() / "home", install::Freshness::Online);
    const install::RegistryPackage& spdlog = *regs.get("").package("spdlog");

    install::service::PackageDetails details{.description = "Fast C++ logging\x1b[31m library\n",
                                             .license = "MIT",
                                             .repository = "https://github.com/gabime/spdlog",
                                             .homepage = "https://spdlog.example",
                                             .owners = {"alice", "bob"},
                                             .downloads = 1234567};
    CHECK_EQ(install::render_info(spdlog, spdlog.versions.front(), details, "cpm install spdlog --registry work"),
             "spdlog 1.17.0 · static library · C++11 · MIT\n"
             "Fast C++ logging [31m library\n"  // control characters from the service become spaces
             "\n"
             "repository    https://github.com/gabime/spdlog\n"
             "homepage      https://spdlog.example\n"
             "owners        alice, bob\n"
             "downloads     1,234,567\n"
             "dependencies  fmt ^12.1.0, zlib ^1.3\n"
             "versions      1.17.0 (latest)\n"
             "\n"
             "install with  cpm install spdlog --registry work\n");

    // The homepage is left out when it is the repository, and so is what the service does not know.
    details = {.repository = "https://github.com/gabime/spdlog", .homepage = "https://github.com/gabime/spdlog"};
    std::string text = install::render_info(spdlog, spdlog.versions.front(), details, "cpm install spdlog");
    CHECK(text.starts_with("spdlog 1.17.0 · static library · C++11\n\nrepository    https://github.com/gabime/spdlog\ndependencies"));
}

TEST(info_lists_at_most_twenty_versions_wrapped_to_the_terminal) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    for (int minor = 0; minor < 25; ++minor) release(reg, "lib", std::format("1.{}.0", minor), entry("header-only"));
    install::Registries regs(config_for(reg), t.path() / "home", install::Freshness::Online);
    const install::RegistryPackage& lib = *regs.get("").package("lib");

    std::string text = install::render_info(lib, lib.versions.front(), std::nullopt, "cpm install lib");
    CHECK(text.starts_with("lib 1.24.0 · header-only library · C++17\n"));
    CHECK(text.find("versions      1.24.0 (latest), 1.23.0,") != std::string::npos);
    CHECK(text.find("1.5.0, and 5 more\n") != std::string::npos);
    CHECK(text.find(" 1.4.0") == std::string::npos);
    for (const auto& line : str::split_lines(text)) {
        CHECK(str::utf8_length(line) <= 80);
        if (!line.empty() && line[0] == ' ') CHECK(line.starts_with("              1."));  // aligned under the first
    }
}

TEST(tree_shows_each_package_under_what_requires_it) {
    lock::Lockfile lock = project_lock();
    CHECK_EQ(install::render_tree(lock, "hello 0.1.0", {}),
             "hello 0.1.0\n"
             "├── fmt 12.2.0\n"
             "├── mylib 0.1.0 (path ../mylib)\n"
             "│   ├── fmt 12.2.0 (*)\n"
             "│   └── spdlog 1.17.0\n"
             "│       └── fmt 12.2.0 (*)\n"
             "├── spdlog 1.17.0 (*)\n"
             "└── doctest 2.4.11 (git https://github.com/doctest/doctest.git#v2.4.11) (dev)\n");
    CHECK_EQ(install::render_tree(lock, "hello 0.1.0", {.depth = 1, .prod = true}),
             "hello 0.1.0\n"
             "├── fmt 12.2.0\n"
             "├── mylib 0.1.0 (path ../mylib)\n"
             "└── spdlog 1.17.0\n");
    CHECK_EQ(install::render_tree(lock, "hello 0.1.0", {.depth = 0}), "hello 0.1.0\n");
    CHECK_EQ(install::render_tree(lock::Lockfile{}, "empty 1.0.0", {}), "empty 1.0.0\n");
}

TEST(why_follows_dependents_up_to_the_project) {
    lock::Lockfile lock = project_lock();
    // What each manifest asks for. mylib's manifest is not on this machine.
    auto requirement_of = [](const lock::Package* dependent, const std::string& name) -> std::optional<std::string> {
        static const std::map<std::pair<std::string, std::string>, std::string> asks = {
            {{"", "fmt"}, "^12.2.0"},
            {{"", "spdlog"}, "^1.17.0"},
            {{"", "mylib"}, "path ../mylib"},
            {{"", "doctest"}, "git https://github.com/doctest/doctest.git#v2.4.11"},
            {{"spdlog", "fmt"}, "^12.1.0"},
        };
        auto it = asks.find({dependent ? dependent->name : "", name});
        return it == asks.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
    CHECK_EQ(install::render_why(lock, "hello 0.1.0", "fmt", requirement_of),
             "fmt 12.2.0\n"
             "├── ^12.2.0 from hello 0.1.0 (the project)\n"
             "├── 12.2.0 from mylib 0.1.0 (path ../mylib)\n"  // no manifest: the locked version
             "│   └── path ../mylib from hello 0.1.0 (the project)\n"
             "└── ^12.1.0 from spdlog 1.17.0\n"
             "    ├── ^1.17.0 from hello 0.1.0 (the project)\n"
             "    └── 1.17.0 from mylib 0.1.0 (path ../mylib) (*)\n");
    CHECK_EQ(install::render_why(lock, "hello 0.1.0", "doctest", requirement_of),
             "doctest 2.4.11 (git https://github.com/doctest/doctest.git#v2.4.11)\n"
             "└── git https://github.com/doctest/doctest.git#v2.4.11 from hello 0.1.0 (the project, dev)\n");
    CHECK_THROWS(install::render_why(lock, "hello 0.1.0", "zlib", requirement_of), "zlib is not in cpm-lock.json");
}

TEST(requirements_read_like_the_manifest) {
    manifest::DependencySpec spec;
    spec.name = "fmt";
    spec.range = "^12.2.0";
    CHECK_EQ(install::requirement(spec), "^12.2.0");
    spec.kind = manifest::DependencySpec::Kind::Git;
    spec.git_url = "https://github.com/doctest/doctest.git";
    spec.git_ref = "v2.4.11";
    CHECK_EQ(install::requirement(spec), "git https://github.com/doctest/doctest.git#v2.4.11");
    spec.git_ref.clear();
    CHECK_EQ(install::requirement(spec), "git https://github.com/doctest/doctest.git");
    spec.kind = manifest::DependencySpec::Kind::Path;
    spec.path = "../mylib";
    CHECK_EQ(install::requirement(spec), "path ../mylib");
}
