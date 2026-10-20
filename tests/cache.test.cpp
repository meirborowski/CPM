// cpm cache: projects.json, what the registered projects use, and cleaning up (docs/spec/store.md §8).

#include <algorithm>

#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/sha256.hpp"
#include "install/cache.hpp"
#include "install/git.hpp"
#include "json/parse.hpp"
#include "lock/lockfile.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"

using namespace cpm;
namespace fs = std::filesystem;
namespace cache = install::cache;
using fixture::write;

namespace {

const std::string kHexA(64, 'a'), kHexB(64, 'b'), kCommit = std::string(40, 'c');
const std::string kGitUrl = "https://example.com/g.git";

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// A complete store entry, with a built library.
void entry(const fs::path& home, const std::string& name, const std::string& integrity, bool complete = true) {
    fs::path dir = home / "store" / name;
    write(dir / "cpm.jsonc", "{}\n");
    write(dir / "include" / "x.hpp", std::string(1000, 'x'));
    write(dir / ".integrity", integrity + "\n");
    write(dir / "lib" / "0123456789abcdef" / "meta.json", "{}\n");
    if (complete) write(dir / ".complete", "");
}

lock::Package package(const std::string& name, lock::Resolution resolution) {
    lock::Package p;
    p.name = name;
    p.version = *semver::Version::parse("1.0.0");
    p.resolution = std::move(resolution);
    return p;
}

// A project using a@1.0.0 and c@1.0.0 from a registry and g@1.0.0 from git.
fs::path project(const fs::path& dir) {
    lock::Lockfile lock;
    auto v = *semver::Version::parse("1.0.0");
    lock.dependencies["a"] = {"^1.0.0", v};
    lock.dependencies["c"] = {"^1.0.0", v};
    lock.dependencies["g"] = {"git+" + kGitUrl + "#v1", v};
    for (auto p : {package("a", lock::RegistryResolution{"https://r.example/", "https://r.example/a.tar.gz", "sha256:" + kHexA}),
                   package("c", lock::RegistryResolution{"https://r.example/", "https://r.example/c.tar.gz", "sha256:" + std::string(64, 'd')}),
                   package("g", lock::GitResolution{kGitUrl, "v1", kCommit})}) {
        lock.packages.emplace(p.key(), p);
    }
    write(dir / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0" })");
    lock::save(dir, lock);
    return dir;
}

// The home of a machine with one live project, one deleted project, and some of everything in the store and caches.
fs::path machine(const fs::path& root) {
    fs::path home = root / "home";
    fs::path app = project(root / "app");
    write(home / "config.jsonc", "{}\n");
    write(home / "credentials.json", "{}\n");
    write(home / "tools" / "ninja" / "ninja.exe", "tool");
    write(home / "store" / ".layout-version", "1\n");
    entry(home, "a@1.0.0", "sha256:" + kHexA);                        // used
    entry(home, "b@1.0.0", "sha256:" + kHexB);                        // used by no project
    entry(home, "c@1.0.0", "sha256:" + std::string(64, 'd'), false);  // used, but never finished: garbage
    entry(home, "g@1.0.0+g" + kCommit.substr(0, 12), "git:" + kCommit);  // used
    write(home / "cache" / "downloads" / ("sha256-" + kHexA + ".tar.gz"), std::string(500, 'a'));
    write(home / "cache" / "downloads" / ("sha256-" + kHexB + ".tar.gz"), std::string(300, 'b'));
    fs::path used_mirror = install::git::mirror_dir(home, kGitUrl);
    write(used_mirror / "HEAD", "ref: refs/heads/main\n");
    fs::path old_mirror = install::git::mirror_dir(home, "https://example.com/old.git");
    write(old_mirror / "HEAD", "ref: refs/heads/main\n");
    write(old_mirror / "config", "[remote \"origin\"]\n\turl = https://example.com/old.git\n");
    write(home / "cache" / "registry" / "0123456789abcdef" / "index" / "a.json", "{}");
    write(home / "tmp" / "old", "x");  // a file: setting a directory's time is not portable
    write(home / "tmp" / "new" / "file", "x");
    fs::last_write_time(home / "tmp" / "old", fs::file_time_type::clock::now() - std::chrono::hours(48));
    write(home / "logs" / "build-a@1.0.0-0123456789abcdef.log", "log");

    cache::register_project(home, app);
    write(root / "gone" / "cpm.jsonc", "{}");
    cache::register_project(home, root / "gone");
    cpm::fsx::remove_tree(root / "gone");
    return home;
}

std::vector<std::string> labels(const std::vector<cache::Item>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.label);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(projects_are_recorded_once_a_day) {
    check::TempDir t;
    fs::path home = t.path() / "home";
    fs::path app = t.path() / "work" / "app";
    fs::create_directories(app);
    std::string key = cache::project_key(app / "src" / "..");
    CHECK_EQ(key, cache::project_key(app));
    CHECK(key.find('\\') == std::string::npos && !key.ends_with('/'));
#ifdef _WIN32
    CHECK(key[0] >= 'A' && key[0] <= 'Z' && key[1] == ':');
#endif

    cache::register_project(home, app);
    auto list = cache::projects(home);
    CHECK_EQ(list.size(), 1u);
    CHECK(list.contains(key) && list[key].size() == 10);  // "2026-10-20"
    auto doc = json::parse(cpm::fsx::read_file(home / "projects.json"), json::kStrict);
    CHECK_EQ(doc.find("projectsVersion")->as_int(), 1);

    // An older date is moved to today; a later one (a clock that was ahead) is left alone.
    write(home / "projects.json", std::format(R"({{"projectsVersion": 1, "projects": {{"{}": {{"lastUsed": "2000-01-01"}}}}}})", key));
    cache::register_project(home, app);
    CHECK(cache::projects(home)[key] > "2000-01-01");
    std::string future = std::format(R"({{"projectsVersion": 1, "projects": {{"{}": {{"lastUsed": "2999-01-01"}}}}}})", key);
    write(home / "projects.json", future);
    cache::register_project(home, app);
    CHECK_EQ(cpm::fsx::read_file(home / "projects.json"), future);

    // A damaged file is started again rather than failing a build.
    write(home / "projects.json", "{ not json");
    cache::register_project(home, app);
    CHECK_EQ(cache::projects(home).size(), 1u);
}

TEST(the_survey_finds_what_no_project_uses) {
    check::TempDir t;
    fs::path home = machine(t.path());
    cache::Survey s = cache::survey(home);
    CHECK_EQ(s.packages, 4u);
    CHECK_EQ(s.libraries, 4u);
    CHECK_EQ(s.archives, 2u);
    CHECK_EQ(s.download_bytes, 800u);
    CHECK_EQ(s.mirrors, 2u);
    CHECK_EQ(s.projects.size(), 1u);
    CHECK_EQ(s.gone.size(), 1u);
    CHECK(s.unreadable.empty());
    CHECK_EQ(labels(s.unused), (std::vector<std::string>{"archive of b@1.0.0", "b@1.0.0", "c@1.0.0", "mirror of https://example.com/old.git",
                                                         "tmp/old"}));
}

TEST(clean_removes_what_no_project_uses) {
    check::TempDir t;
    fs::path home = machine(t.path());

    cache::CleanResult dry = cache::clean(home, false, true);
    CHECK_EQ(labels(dry.removed).size(), 5u);
    CHECK_EQ(dry.forgotten.size(), 1u);
    CHECK(present(home / "store" / "b@1.0.0"));  // a dry run changes nothing
    CHECK_EQ(cache::projects(home).size(), 2u);

    // Another cpm process holds b's library lock: b stays, the rest goes.
    FileLock held = FileLock::acquire(home / "locks" / (Sha256::hex("b@1.0.0 for abi 0123456789abcdef").substr(0, 16) + ".lock"), "test");
    cache::CleanResult r = cache::clean(home, false, false);
    CHECK_EQ(labels(r.busy), (std::vector<std::string>{"b@1.0.0"}));
    CHECK_EQ(labels(r.removed), (std::vector<std::string>{"archive of b@1.0.0", "c@1.0.0", "mirror of https://example.com/old.git", "tmp/old"}));
    CHECK(r.bytes > 300);
    CHECK_EQ(r.forgotten.size(), 1u);
    CHECK_EQ(cache::projects(home).size(), 1u);
    CHECK(present(home / "store" / "b@1.0.0" / ".complete"));
    CHECK(!present(home / "store" / "c@1.0.0"));
    CHECK(!present(home / "cache" / "downloads" / ("sha256-" + kHexB + ".tar.gz")));
    CHECK(!present(home / "tmp" / "old"));
    // What the project uses stays.
    CHECK(present(home / "store" / "a@1.0.0" / ".complete"));
    CHECK(present(home / "store" / ("g@1.0.0+g" + kCommit.substr(0, 12))));
    CHECK(present(home / "cache" / "downloads" / ("sha256-" + kHexA + ".tar.gz")));
    CHECK(present(install::git::mirror_dir(home, kGitUrl) / "HEAD"));
    CHECK(present(home / "tmp" / "new"));

    held = FileLock::acquire(home / "locks" / "unrelated.lock", "test");  // releases b's lock
    r = cache::clean(home, false, false);
    CHECK_EQ(labels(r.removed), (std::vector<std::string>{"b@1.0.0"}));
    CHECK(cache::clean(home, false, false).removed.empty());
}

TEST(clean_all_empties_the_store_and_caches_but_keeps_the_rest) {
    check::TempDir t;
    fs::path home = machine(t.path());
    cache::CleanResult r = cache::clean(home, true, false);
    CHECK(r.busy.empty());
    CHECK(r.removed.size() >= 10);
    CHECK(cpm::fsx::list_dir(home / "store").size() == 1);  // only .layout-version
    CHECK(present(home / "store" / ".layout-version"));
    CHECK(cpm::fsx::list_dir(home / "cache" / "downloads").empty());
    CHECK(cpm::fsx::list_dir(home / "cache" / "git").empty());
    CHECK(!present(home / "cache" / "registry"));
    CHECK(cpm::fsx::list_dir(home / "logs").empty());
    for (const auto& e : cpm::fsx::list_dir(home / "tmp")) CHECK(e.name.starts_with("trash-") == false);
    CHECK(present(home / "config.jsonc"));
    CHECK(present(home / "credentials.json"));
    CHECK(present(home / "projects.json"));
    CHECK(present(home / "tools" / "ninja" / "ninja.exe"));
}
