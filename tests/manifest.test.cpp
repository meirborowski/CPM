#include "core/fsx.hpp"
#include "manifest/config.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"

using namespace cpm;
using manifest::DependencySpec;
using manifest::TargetKind;

static manifest::Manifest parse(std::string text) {
    return manifest::parse(std::move(text), "/p/cpm.jsonc", "cpm.jsonc");
}

// The diagnostics a manifest produces, or an empty list if it parses.
static std::vector<Diagnostic> problems(std::string text) {
    try {
        parse(std::move(text));
        return {};
    } catch (const DiagnosticsError& e) {
        return e.diagnostics();
    }
}

static bool has_problem(const std::vector<Diagnostic>& ds, std::string_view needle) {
    for (const auto& d : ds) {
        if (d.severity == Severity::Error && d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

TEST(minimal_manifest_gets_defaults) {
    auto m = parse(R"({ "name": "hello", "version": "0.1.0" })");
    CHECK_EQ(m.name, "hello");
    CHECK_EQ(m.version.to_string(), "0.1.0");
    CHECK_EQ(m.std, "c++20");
    CHECK(!m.type.has_value());
    CHECK_EQ(m.include_dir, "include");
    CHECK(m.profiles.contains("debug"));
    CHECK(m.profiles.contains("release"));
    CHECK_EQ(m.profiles.at("release").opt, "3");
    CHECK(!m.profiles.at("release").assertions);
    CHECK(m.warnings.empty());
}

TEST(loads_the_spec_example) {
    auto m = manifest::load("docs/spec/examples/hello/cpm.jsonc", ".");
    CHECK_EQ(m.name, "hello");
    CHECK(m.type == TargetKind::Executable);
    CHECK_EQ(m.std, "c++23");
    CHECK_EQ(m.dependencies.size(), 3u);
    CHECK_EQ(m.dev_dependencies.size(), 1u);
    const DependencySpec& fmt = m.dependencies[0];
    CHECK(fmt.kind == DependencySpec::Kind::Registry);
    CHECK_EQ(fmt.range, "^10.2.0");
    const DependencySpec& json = m.dependencies[1];
    CHECK(json.kind == DependencySpec::Kind::Git);
    CHECK_EQ(json.git_ref, "v3.11.3");
    CHECK(json.overlay.has_value());
    // The canonical specifier recorded in the lockfile (dependencies.md §1.2).
    CHECK_EQ(json.specifier, R"({"git":"https://github.com/nlohmann/json.git","tag":"v3.11.3"})");
    const DependencySpec& mylib = m.dependencies[2];
    CHECK(mylib.kind == DependencySpec::Kind::Path);
    CHECK_EQ(mylib.path, "../mylib");
    CHECK(m.dev_dependencies[0].dev);
    // Profiles: debug patched, custom inherits release.
    CHECK_EQ(m.profiles.at("debug").sanitizers, (std::vector<std::string>{"address", "undefined"}));
    CHECK(m.profiles.at("release").lto);
    const auto& prof = m.profiles.at("profiling");
    CHECK(prof.debug);
    CHECK(prof.lto);
    CHECK_EQ(prof.opt, "3");
    auto win = manifest::effective_platform(m, Os::Windows);
    CHECK_EQ(win.system_libs, (std::vector<std::string>{"ws2_32"}));
    auto linux_ = manifest::effective_platform(m, Os::Linux);
    CHECK_EQ(linux_.system_libs, (std::vector<std::string>{"pthread"}));
}

TEST(git_shorthands) {
    auto m = parse(R"({ "name": "a", "version": "1.0.0", "dependencies": {
        "d": "github:doctest/doctest#v2.4.11", "e": "git+https://x.org/e.git" } })");
    CHECK_EQ(m.dependencies[0].git_url, "https://github.com/doctest/doctest.git");
    CHECK_EQ(m.dependencies[0].git_ref, "v2.4.11");
    CHECK_EQ(m.dependencies[1].git_url, "https://x.org/e.git");
    CHECK_EQ(m.dependencies[1].git_ref, "");
}

TEST(reports_all_errors_with_locations) {
    auto ds = problems(R"({
  "name": "Hello",
  "version": "v1.0.0",
  "type": "exe",
  "std": "c++03"
})");
    CHECK(has_problem(ds, "invalid package name \"Hello\""));
    CHECK(has_problem(ds, "invalid version \"v1.0.0\""));
    CHECK(has_problem(ds, "unknown target type \"exe\""));
    CHECK(has_problem(ds, "unknown C++ standard \"c++03\""));
    CHECK_EQ(ds.size(), 4u);
    CHECK(ds[0].loc.has_value());
    CHECK_EQ(ds[0].loc->line, 2u);
    CHECK_EQ(ds[0].loc->col, 11u);
    CHECK_EQ(ds[2].loc->line, 4u);
}

TEST(required_fields_and_types) {
    CHECK(has_problem(problems("{}"), "missing required field \"name\""));
    CHECK(has_problem(problems("{}"), "missing required field \"version\""));
    CHECK(has_problem(problems("[]"), "must be a JSON object"));
    CHECK(has_problem(problems(R"({"name": 5, "version": "1.0.0"})"), "\"name\" must be a string"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "defines": ["1X"]})"), "invalid define"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "systemLibs": ["-lm"]})"),
                      "invalid system library"));
    CHECK(has_problem(problems(R"({"name": "con", "version": "1.0.0"})"), "invalid package name"));
    CHECK(has_problem(problems("{\"name\": \"a\",\n \"name\": \"b\"}"), "duplicate key"));
}

TEST(dependency_rules) {
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "dependencies": {"b": "1"},
                                   "devDependencies": {"b": "1"}})"),
                      "both dependencies and devDependencies"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "dependencies": {"a": "1"}})"),
                      "cannot depend on itself"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "dependencies": {"b": {"git": "u", "tag": "t", "rev": "abcdef1"}}})"),
                      "at most one of"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "dependencies": {"b": {}}})"),
                      "needs \"version\", \"git\" or \"path\""));
}

TEST(unknown_keys_warn_with_suggestions) {
    auto m = parse(R"({ "name": "a", "version": "1.0.0", "dependancies": {} })");
    CHECK_EQ(m.warnings.size(), 1u);
    CHECK(m.warnings[0].message.find("unknown key \"dependancies\"") != std::string::npos);
    CHECK_EQ(m.warnings[0].hint, "did you mean \"dependencies\"?");
}

TEST(profile_rules) {
    auto m = parse(R"({ "name": "a", "version": "1.0.0", "profiles": {
        "debug": { "defines": ["X"], "opt": 1 },
        "fast": { "inherits": "debug", "defines": ["Y", "X"], "opt": "s" } } })");
    CHECK_EQ(m.profiles.at("debug").opt, "1");
    CHECK_EQ(m.profiles.at("fast").defines, (std::vector<std::string>{"X", "Y"}));  // arrays append, deduped
    CHECK_EQ(m.profiles.at("fast").opt, "s");
    CHECK(m.profiles.at("fast").debug);

    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "profiles": {"x": {}}})"), "needs \"inherits\""));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "profiles": {
        "x": {"inherits": "y"}, "y": {"inherits": "x"}}})"), "cycle"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "profiles": {"x": {"inherits": "nope"}}})"),
                      "unknown profile \"nope\""));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "profiles": {"debug": {"opt": 4}}})"),
                      "\"opt\" must be"));
    CHECK(has_problem(problems(R"({"name": "a", "version": "1.0.0", "profiles": {"debug": {"sanitizers": ["fast"]}}})"),
                      "unknown sanitizer"));
}

TEST(user_config) {
    check::TempDir home;
    auto empty = manifest::load_user_config(home.path());
    CHECK(!empty.cxx.has_value());
    fsx::write_file_atomic(home.path() / "config.jsonc", R"({ // comment
  "registries": { "default": "https://r.example/", "local": { "url": "file:///c/r/", "allowHttp": true } },
  "cxx": "clang++", "jobs": 4, "colour": "never" })");
    auto cfg = manifest::load_user_config(home.path());
    CHECK_EQ(cfg.cxx.value_or(""), "clang++");
    CHECK_EQ(cfg.jobs.value_or(0), 4);
    CHECK_EQ(cfg.registries.at("default").url, "https://r.example/");
    CHECK(cfg.registries.at("local").allow_http);
    CHECK_EQ(cfg.warnings.size(), 1u);  // "colour"
}
