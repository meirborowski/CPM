// The PubGrub resolver (docs/spec/dependencies.md §4.2 to §4.4): what it explains when no solution exists, the
// cases it must still solve, and large generated graphs.

#include <chrono>
#include <map>

#include "install/registry.hpp"
#include "install/resolver.hpp"
#include "install/sources.hpp"
#include "lock/lockfile.hpp"
#include "semver/range.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"

using namespace cpm;
namespace fs = std::filesystem;
using json::Value;

namespace {

using fixture::Index;

manifest::UserConfig config_for(const Index& index) {
    manifest::UserConfig c;
    c.registries["default"] = {index.url(), false};
    return c;
}

manifest::Manifest project(const fs::path& dir, const std::string& deps, const std::string& dev = "{}") {
    fixture::write(dir / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++20", "dependencies": )" + deps +
                                          R"(, "devDependencies": )" + dev + " }\n");
    return manifest::load(dir / "cpm.jsonc", dir);
}

install::Resolution resolve(const Index& index, const fs::path& home, const manifest::Manifest& m,
                            const lock::Lockfile* lock = nullptr) {
    install::Registries regs(config_for(index), home, install::Freshness::Online);
    install::Sources sources(m.root, home, regs, false);
    return install::resolve(m, sources, lock);
}

std::map<std::string, std::string> versions(const install::Resolution& res) {
    std::map<std::string, std::string> out;
    for (const auto& [name, p] : res.packages) out[name] = p.meta.version.to_string();
    return out;
}

// The error, and its hint, when resolution fails.
struct Failure {
    std::string message, hint;
};

Failure failure(const Index& index, const fs::path& home, const manifest::Manifest& m) {
    try {
        resolve(index, home, m);
    } catch (const Error& e) {
        return {e.what(), e.hint()};
    }
    return {"(resolved)", ""};
}

}  // namespace

TEST(explains_a_conflict_through_a_dependency) {
    check::TempDir t;
    Index index(t.path() / "registry");
    for (const char* v : {"9.1.0", "10.0.0", "10.2.1"}) index.add("fmt", v);
    index.add("spdlog", "1.11.0", {{"fmt", "^8.0.0"}});
    for (const char* v : {"1.12.0", "1.13.0", "1.14.0"}) index.add("spdlog", v, {{"fmt", "^9.1.0"}});
    index.save();

    Failure f = failure(index, t.path() / "home", project(t.path() / "app", R"({ "fmt": "^10.2.0", "spdlog": "^1.12.0" })"));
    CHECK_EQ(f.message,
             "the project's dependencies can't be resolved\n"
             "  Because spdlog >=1.12.0 depends on fmt ^9.1.0 and the project depends on fmt ^10.2.0, spdlog >=1.12.0 can't be used.\n"
             "  So, because the project depends on spdlog ^1.12.0, no versions satisfy every requirement.");
    CHECK_EQ(f.hint, "change the range of fmt or spdlog in cpm.jsonc; cpm info <package> shows what each version depends on");

    // With fmt left open, spdlog's requirement decides it.
    auto res = resolve(index, t.path() / "home", project(t.path() / "app", R"({ "fmt": ">=9", "spdlog": "^1.12.0" })"));
    CHECK_EQ(versions(res), (std::map<std::string, std::string>{{"fmt", "9.1.0"}, {"spdlog", "1.14.0"}}));
}

TEST(explains_ranges_that_match_no_version) {
    check::TempDir t;
    Index index(t.path() / "registry");
    index.add("b", "1.0.0").add("b", "1.1.0").add("b", "2.0.0").add("b", "2.1.0", {}, true);  // 2.1.0 is yanked
    index.add("a", "1.0.0", {{"b", "^3.0.0"}});
    index.save();
    fs::path home = t.path() / "home";

    Failure direct = failure(index, home, project(t.path() / "app", R"({ "b": "^2.1.0" })"));
    CHECK_EQ(direct.message,
             "the project's dependencies can't be resolved\n"
             "  The project depends on b ^2.1.0, which matches no version (available: 2.0.0, 1.1.0, 1.0.0).");
    CHECK_EQ(direct.hint, "change the range of b in cpm.jsonc");

    Failure transitive = failure(index, home, project(t.path() / "app", R"({ "a": "^1.0.0" })"));
    CHECK_EQ(transitive.message,
             "the project's dependencies can't be resolved\n"
             "  Because the project depends on a ^1.0.0 and a 1.0.0 depends on b ^3.0.0, which matches no version (available: "
             "2.0.0, 1.1.0, 1.0.0), no versions satisfy every requirement.");
}

TEST(versions_that_need_a_missing_package_are_skipped) {
    check::TempDir t;
    Index index(t.path() / "registry");
    index.add("a", "1.0.0").add("a", "2.0.0", {{"ghost", "^1.0.0"}});
    index.save();
    fs::path home = t.path() / "home";
    // a 2.0.0 depends on a package the registry doesn't have, so a 1.0.0 it is.
    CHECK_EQ(versions(resolve(index, home, project(t.path() / "app", R"({ "a": "*" })"))).at("a"), "1.0.0");

    Failure f = failure(index, home, project(t.path() / "app", R"({ "a": "^2.0.0" })"));
    CHECK_EQ(f.message, std::format("the project's dependencies can't be resolved\n"
                                    "  Because the project depends on a ^2.0.0 and a 2.0.0 depends on ghost, which {} doesn't "
                                    "have, no versions satisfy every requirement.",
                                    index.base()));
    // The project's own dependencies must exist.
    CHECK_THROWS(resolve(index, home, project(t.path() / "app", R"({ "ghost": "^1.0.0" })")), "has no package \"ghost\"");
}

TEST(numbers_conclusions_that_are_used_twice) {
    // The branching example from PubGrub's documentation: both foo 1.0.0 and foo 1.1.0 fail, for different reasons.
    check::TempDir t;
    Index index(t.path() / "registry");
    index.add("foo", "1.0.0", {{"a", "^1.0.0"}, {"b", "^1.0.0"}});
    index.add("foo", "1.1.0", {{"x", "^1.0.0"}, {"y", "^1.0.0"}});
    index.add("a", "1.0.0", {{"b", "^2.0.0"}});
    index.add("b", "1.0.0").add("b", "2.0.0");
    index.add("x", "1.0.0", {{"y", "^2.0.0"}});
    index.add("y", "1.0.0").add("y", "2.0.0");
    index.save();

    Failure f = failure(index, t.path() / "home", project(t.path() / "app", R"({ "foo": "^1.0.0" })"));
    CHECK_EQ(f.message,
             "the project's dependencies can't be resolved\n"
             "  Because foo 1.0.0 depends on a ^1.0.0 and a 1.0.0 depends on b ^2.0.0, foo 1.0.0 depends on b ^2.0.0.\n"
             "  (1) And because foo 1.0.0 depends on b ^1.0.0, foo 1.0.0 can't be used.\n"
             "\n"
             "  Because foo 1.1.0 depends on x ^1.0.0 and x 1.0.0 depends on y ^2.0.0, foo 1.1.0 depends on y ^2.0.0.\n"
             "  And because foo 1.1.0 depends on y ^1.0.0, foo 1.1.0 can't be used.\n"
             "  And because foo 1.0.0 can't be used (1), foo can't be used.\n"
             "  So, because the project depends on foo ^1.0.0, no versions satisfy every requirement.");
}

TEST(keeps_locked_versions_and_explains_path_packages) {
    check::TempDir t;
    Index index(t.path() / "registry");
    for (const char* v : {"1.0.0", "1.1.0", "1.2.0"}) index.add("a", v, {{"b", "^1.0.0"}});
    for (const char* v : {"1.0.0", "1.5.0"}) index.add("b", v);
    index.save();
    fs::path home = t.path() / "home";
    auto m = project(t.path() / "app", R"({ "a": "^1.0.0", "b": "^1.0.0" })");

    // Every package pinned: the lockfile's versions are kept, even when newer ones exist.
    lock::Lockfile lock;
    for (auto [name, version] : {std::pair{"a", "1.1.0"}, std::pair{"b", "1.0.0"}}) {
        lock::Package p;
        p.name = name;
        p.version = *semver::Version::parse(version);
        p.resolution = lock::RegistryResolution{index.base(), "", "sha256:" + std::string(64, '0')};
        lock.packages.emplace(p.key(), p);
    }
    CHECK_EQ(versions(resolve(index, home, m, &lock)), (std::map<std::string, std::string>{{"a", "1.1.0"}, {"b", "1.0.0"}}));
    CHECK_EQ(versions(resolve(index, home, m)), (std::map<std::string, std::string>{{"a", "1.2.0"}, {"b", "1.5.0"}}));

    // A path package that the project and a registry package disagree about.
    fixture::write(t.path() / "b" / "cpm.jsonc", R"({ "name": "b", "version": "2.0.0", "type": "header-only" })");
    Failure f = failure(index, home, project(t.path() / "app", R"({ "a": "^1.0.0", "b": "file:../b" })"));
    CHECK_EQ(f.message,
             "the project's dependencies can't be resolved\n"
             "  Because the project depends on a ^1.0.0 and every version of a depends on b ^1.0.0, which doesn't match b 2.0.0 "
             "(path ../b), no versions satisfy every requirement.");
    CHECK_EQ(f.hint, "change the range of a or b in cpm.jsonc; cpm info <package> shows what each version depends on");
}

TEST(conflicts_are_found_without_trying_every_combination) {
    // Fifteen packages with ten versions each come first by name, and a package after them can never be satisfied.
    // A backtracking solver would try all 10^15 combinations before giving up; PubGrub learns that z alone fails.
    check::TempDir t;
    Index index(t.path() / "registry");
    std::string deps = "{";
    for (int i = 0; i < 15; ++i) {
        std::string name = std::format("p{:02}", i);
        for (int v = 0; v < 10; ++v) index.add(name, std::format("1.{}.0", v), {{"q", "^1.0.0"}});
        deps += std::format(R"("{}": "^1.0.0", )", name);
    }
    for (int v = 0; v < 10; ++v) index.add("q", std::format("1.{}.0", v));
    index.add("z", "1.0.0", {{"q", "^2.0.0"}});
    index.save();
    deps += R"("z": "^1.0.0" })";

    auto start = std::chrono::steady_clock::now();
    Failure f = failure(index, t.path() / "home", project(t.path() / "app", deps));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
    CHECK_EQ(f.message,
             "the project's dependencies can't be resolved\n"
             "  Because the project depends on z ^1.0.0 and z 1.0.0 depends on q ^2.0.0, which matches no version (available: "
             "1.9.0, 1.8.0, 1.7.0, 1.6.0, 1.5.0, 1.4.0, 1.3.0, 1.2.0, 1.1.0, 1.0.0), no versions satisfy every requirement.");
}

TEST(large_graphs_resolve_to_consistent_versions) {
    // 300 packages in a chain of layers, 10 versions each. Every version depends on up to three packages further
    // down, with ranges that bound the newest version they accept, so the newest versions don't all fit together.
    check::TempDir t;
    Index index(t.path() / "registry");
    constexpr int kPackages = 300, kVersions = 10;
    std::uint64_t seed = 42;
    auto next = [&](std::uint64_t n) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return (seed >> 33) % n;
    };
    for (int p = 0; p < kPackages; ++p) {
        for (int v = 0; v < kVersions; ++v) {
            std::map<std::string, std::string> deps;
            for (int d = 0; d < 3 && p + 1 < kPackages; ++d) {
                int target = p + 1 + static_cast<int>(next(std::min<std::uint64_t>(20, kPackages - p - 1)));
                deps[std::format("p{:03}", target)] = std::format(">=1.0.0 <=1.{}.0", next(kVersions));
            }
            index.add(std::format("p{:03}", p), std::format("1.{}.0", v), deps);
        }
    }
    index.save();
    auto m = project(t.path() / "app", R"({ "p000": "^1.0.0", "p001": "^1.0.0", "p002": "^1.0.0" })");

    auto start = std::chrono::steady_clock::now();
    install::Resolution res = resolve(index, t.path() / "home", m);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(60));
    CHECK(res.packages.size() > 50);
    CHECK_EQ(res.packages.at("p000").meta.version.to_string(), "1.9.0");
    // Every requirement of every chosen version holds.
    for (const auto& [name, p] : res.packages) {
        for (const auto& d : p.meta.manifest.dependencies) {
            auto it = res.packages.find(d.name);
            CHECK(it != res.packages.end());
            if (it != res.packages.end()) CHECK(semver::Range::parse(d.range)->contains(it->second.meta.version));
        }
    }
    // The same inputs give the same result.
    CHECK(versions(resolve(index, t.path() / "home", m)) == versions(res));
}
