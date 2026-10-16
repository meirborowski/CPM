#include "core/fsx.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

namespace {

const fs::path kExample = "docs/spec/examples/hello";  // tests run from the project root

manifest::Manifest read_path(const lock::Package& p) {
    const auto& res = std::get<lock::PathResolution>(p.resolution);
    return manifest::load(kExample / res.path / "cpm.jsonc", ".");
}

std::string example_text() { return fsx::read_file(kExample / "cpm-lock.json"); }

}  // namespace

TEST(parses_the_spec_example) {
    lock::Lockfile lock = lock::parse(example_text(), "cpm-lock.json");
    CHECK_EQ(lock.dependencies.size(), 3u);
    CHECK_EQ(lock.dev_dependencies.size(), 1u);
    CHECK_EQ(lock.packages.size(), 4u);
    const lock::Package* json = lock.find("json");
    CHECK(json != nullptr);
    if (!json) return;
    const auto& git = std::get<lock::GitResolution>(json->resolution);
    CHECK_EQ(git.ref, "v3.11.3");
    CHECK_EQ(git.commit.size(), 40u);
    CHECK(json->type == manifest::TargetKind::HeaderOnly);
    CHECK(lock.find("doctest")->dev);
    CHECK_EQ(lock.find("mylib")->dependencies.at("fmt").to_string(), "10.2.1");
}

TEST(serializes_byte_for_byte) {
    std::string text = example_text();
    CHECK_EQ(lock::serialize(lock::parse(text, "cpm-lock.json")), text);
    lock::Lockfile empty;
    CHECK_EQ(lock::serialize(empty),
             "{\n  \"lockfileVersion\": 1,\n  \"importers\": {\n    \".\": {\n      \"dependencies\": {},\n"
             "      \"devDependencies\": {}\n    }\n  },\n  \"packages\": {}\n}\n");
}

TEST(fresh_for_its_manifest) {
    lock::Lockfile lock = lock::parse(example_text(), "cpm-lock.json");
    manifest::Manifest m = manifest::load(kExample / "cpm.jsonc", ".");
    auto why = lock::staleness(lock, m, read_path);
    CHECK(!why.has_value());
}

TEST(stale_when_the_manifest_changes) {
    lock::Lockfile lock = lock::parse(example_text(), "cpm-lock.json");
    std::string text = fsx::read_file(kExample / "cpm.jsonc");
    auto stale_after = [&](std::string from, std::string to) {
        std::string changed = text;
        auto pos = changed.find(from);
        if (pos == std::string::npos) throw std::runtime_error("pattern not found: " + from);
        changed.replace(pos, from.size(), to);
        manifest::Manifest m = manifest::parse(changed, fs::absolute(kExample / "cpm.jsonc"), "cpm.jsonc");
        return lock::staleness(lock, m, read_path);
    };
    auto changed_spec = stale_after("\"^10.2.0\"", "\"^10.3.0\"");
    CHECK(changed_spec.has_value() && changed_spec->contains("specifier of \"fmt\" changed"));
    auto added = stale_after("\"doctest\": \"^2.4.11\",", "\"doctest\": \"^2.4.11\", \"extra\": \"^1\",");
    CHECK(added.has_value() && added->contains("\"extra\" is not in the lockfile"));
    auto removed = stale_after("\"mylib\": \"file:../mylib\",", "");
    CHECK(removed.has_value() && removed->contains("\"mylib\" was removed"));
}

TEST(stale_when_a_path_package_changes) {
    lock::Lockfile lock = lock::parse(example_text(), "cpm-lock.json");
    manifest::Manifest m = manifest::load(kExample / "cpm.jsonc", ".");
    auto bumped = [](const lock::Package& p) {
        manifest::Manifest mm = read_path(p);
        mm.version = *semver::Version::parse("0.4.0");
        return mm;
    };
    auto why = lock::staleness(lock, m, bumped);
    CHECK(why.has_value() && why->contains("changed its version"));
}

TEST(rejects_corrupt_and_newer_lockfiles) {
    std::string text = example_text();
    std::string newer = text;
    newer.replace(newer.find("\"lockfileVersion\": 1"), 20, "\"lockfileVersion\": 2");
    CHECK_THROWS(lock::parse(newer, "cpm-lock.json"), "lockfileVersion 2");

    std::string bad_hash = text;
    bad_hash.replace(bad_hash.find("sha256:d19641fd"), 15, "sha256:XYZ");
    CHECK_THROWS(lock::parse(bad_hash, "cpm-lock.json"), "integrity");

    CHECK_THROWS(lock::parse("{ // no comments\n}", "cpm-lock.json"), "not valid JSON");

    // A dangling reference is corruption, not staleness.
    lock::Lockfile lock = lock::parse(text, "cpm-lock.json");
    lock.packages.erase("fmt@10.2.1");
    manifest::Manifest m = manifest::load(kExample / "cpm.jsonc", ".");
    CHECK_THROWS(lock::staleness(lock, m, read_path), "fmt@10.2.1");
}
