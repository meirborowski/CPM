#include <deque>
#include <fstream>

#include "build/layout.hpp"
#include "build/pipeline.hpp"
#include "build/tests.hpp"
#include "core/fsx.hpp"
#include "core/proc.hpp"
#include "init/init.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"

// cpm::proc is spelled out: macOS declares a global `struct proc` (<sys/proc.h>), so plain `proc::` is ambiguous.
using namespace cpm;
namespace fs = std::filesystem;
using manifest::TargetKind;

namespace {

init::Options options_for(const fs::path& dir, std::string name, TargetKind kind) {
    init::Options o;
    o.dir = dir;
    o.name = std::move(name);
    o.kind = kind;
    return o;
}

std::vector<std::string> paths_of(const std::vector<init::File>& files) {
    std::vector<std::string> out;
    for (const auto& f : files) out.push_back(f.path);
    return out;
}

// Scripted answers for ask_questions; runs out like end of input.
struct Script {
    std::deque<std::string> answers;
    std::vector<std::string> prompts, said;
    init::Ask ask() {
        return [this](const std::string& prompt) -> std::optional<std::string> {
            prompts.push_back(prompt);
            if (answers.empty()) return std::nullopt;
            std::string a = answers.front();
            answers.pop_front();
            return a;
        };
    }
    init::Say say() {
        return [this](const std::string& message) { said.push_back(message); };
    }
};

}  // namespace

TEST(derives_names_from_directories) {
    CHECK_EQ(init::name_from_directory("hello"), "hello");
    CHECK_EQ(init::name_from_directory("Hello World"), "hello-world");
    CHECK_EQ(init::name_from_directory("My_App.v2"), "my_app.v2");
    CHECK_EQ(init::name_from_directory("  --x!!y--  "), "x-y");
    CHECK_EQ(init::name_from_directory("2048"), "2048");
    CHECK_EQ(init::name_from_directory("\xC3\x9C" "ber"), "ber");  // non-ASCII is dropped
    CHECK_EQ(init::name_from_directory("CON"), "");                // Windows device name
    CHECK_EQ(init::name_from_directory("!!!"), "");
    CHECK_EQ(init::name_from_directory(""), "");
    CHECK_EQ(init::name_from_directory(std::string(70, 'a')).size(), 64u);
}

TEST(makes_cpp_identifiers) {
    CHECK_EQ(init::identifier("hello"), "hello");
    CHECK_EQ(init::identifier("my-app.core"), "my_app_core");
    CHECK_EQ(init::identifier("9lives"), "_9lives");
    CHECK_EQ(init::identifier("new"), "new_");
    CHECK_EQ(init::identifier("std"), "std_");
}

TEST(renders_files_per_kind) {
    fs::path dir = "/x/hello";
    CHECK_EQ(paths_of(init::render(options_for(dir, "hello", TargetKind::Executable))),
             (std::vector<std::string>{"cpm.jsonc", "src/main.cpp", "src/greeting.hpp", "src/greeting.cpp",
                                       "tests/greeting.test.cpp", ".gitignore"}));
    CHECK_EQ(paths_of(init::render(options_for(dir, "mylib", TargetKind::StaticLib))),
             (std::vector<std::string>{"cpm.jsonc", "include/mylib/mylib.hpp", "src/lib.cpp", "tests/mylib.test.cpp",
                                       ".gitignore"}));
    CHECK_EQ(paths_of(init::render(options_for(dir, "hdr", TargetKind::HeaderOnly))),
             (std::vector<std::string>{"cpm.jsonc", "include/hdr/hdr.hpp", "tests/hdr.test.cpp", ".gitignore"}));
}

TEST(generated_manifest_parses) {
    init::Options o = options_for("/x/p", "my-app", TargetKind::SharedLib);
    o.description = "Says \"hi\"\\";
    o.license = "MIT";
    o.std = "c++20";
    std::string text = init::render(o).front().content;
    manifest::Manifest m = manifest::parse(text, "/x/p/cpm.jsonc", "cpm.jsonc");
    CHECK_EQ(m.name, "my-app");
    CHECK_EQ(m.version.to_string(), "0.1.0");
    CHECK_EQ(m.description, "Says \"hi\"\\");
    CHECK_EQ(m.license, "MIT");
    CHECK_EQ(m.std, "c++20");
    CHECK(m.type == TargetKind::SharedLib);
    CHECK(m.dependencies.empty());
    CHECK(m.warnings.empty());
}

TEST(written_packages_have_the_inferred_layout) {
    for (TargetKind kind : {TargetKind::Executable, TargetKind::StaticLib, TargetKind::SharedLib, TargetKind::HeaderOnly}) {
        check::TempDir dir;
        init::Result r = init::write(options_for(dir.path() / "pkg", "pkg", kind));
        CHECK(r.skipped.empty());
        manifest::Manifest m = manifest::load(dir.path() / "pkg" / "cpm.jsonc", dir.path());
        build::Layout layout = build::infer_layout(m, {});
        CHECK(layout.kind == kind);
        CHECK_EQ(layout.tests.size(), 1u);
    }
}

TEST(never_overwrites) {
    check::TempDir dir;
    fs::path main = dir.path() / "src" / "main.cpp";
    fsx::write_file_atomic(main, "// mine\n");
    fsx::write_file_atomic(dir.path() / ".gitignore", "build/\ncpm_modules/");  // no trailing newline
    init::Result r = init::write(options_for(dir.path(), "p", TargetKind::Executable));
    CHECK_EQ(fsx::read_file(main), "// mine\n");
    CHECK_EQ(r.skipped, (std::vector<std::string>{"src/main.cpp"}));
    CHECK(r.gitignore_updated);
    CHECK_EQ(fsx::read_file(dir.path() / ".gitignore"), "build/\ncpm_modules/\n/build/\ncompile_commands.json\n");

    // A second init refuses because cpm.jsonc exists, and changes nothing.
    fsx::write_file_atomic(dir.path() / "cpm.jsonc", "{}");
    CHECK_THROWS(init::write(options_for(dir.path(), "p", TargetKind::Executable)), "already has a cpm.jsonc");
    CHECK_EQ(fsx::read_file(dir.path() / "cpm.jsonc"), "{}");

    // --force replaces cpm.jsonc only.
    init::Options forced = options_for(dir.path(), "p", TargetKind::Executable);
    forced.force = true;
    init::Result again = init::write(forced);
    CHECK(again.overwrote_manifest);
    CHECK_EQ(again.created, (std::vector<std::string>{"cpm.jsonc"}));
    CHECK_EQ(fsx::read_file(main), "// mine\n");
    CHECK(!again.gitignore_updated);  // already complete
}

TEST(asks_questions_with_defaults_and_validation) {
    init::Options o = options_for("/x/hello", "hello", TargetKind::Executable);
    Script s;
    s.answers = {"Bad Name", "", "1.0", "2.0.0", "  A tool  ", "lib", "c++99", "c++17", "Apache-2.0"};
    init::ask_questions(o, {}, s.ask(), s.say());
    CHECK_EQ(o.name, "hello");  // invalid answer, then empty = default
    CHECK_EQ(o.version, "2.0.0");
    CHECK_EQ(o.description, "A tool");
    CHECK(o.kind == TargetKind::StaticLib);
    CHECK_EQ(o.std, "c++17");
    CHECK_EQ(o.license, "Apache-2.0");
    CHECK_EQ(s.said.size(), 3u);  // name, version, std
    CHECK_EQ(s.prompts.front(), "package name: (hello) ");
}

TEST(questions_skip_fixed_values_and_stop_at_end_of_input) {
    init::Options o = options_for("/x/y", "given", TargetKind::HeaderOnly);
    Script s;
    s.answers = {"3.1.4"};
    init::ask_questions(o, {.name = true, .kind = true, .std = false}, s.ask(), s.say());
    CHECK_EQ(o.version, "3.1.4");
    CHECK(o.kind == TargetKind::HeaderOnly);
    CHECK_EQ(o.name, "given");
    CHECK_EQ(s.prompts.size(), 2u);  // version, then description hit the end of input
    CHECK_EQ(o.std, "c++23");

    init::Options nameless = options_for("/x/!!!", "", TargetKind::Executable);
    Script empty;
    CHECK_THROWS(init::ask_questions(nameless, {}, empty.ask(), empty.say()), "no package name");
}

// End to end: every template builds with the real toolchain and passes its own test.
TEST(scaffolded_packages_build_and_pass_their_tests) {
    for (TargetKind kind : {TargetKind::Executable, TargetKind::StaticLib, TargetKind::SharedLib, TargetKind::HeaderOnly}) {
        check::TempDir dir;
        fs::path root = dir.path() / "pkg";
        init::write(options_for(root, "pkg", kind));

        build::BuildRequest req;
        req.cwd = root;
        req.targets = build::BuildRequest::Targets::Tests;
        build::BuildResult result = build::run_build(req, {});
        CHECK_EQ(result.exit_code, 0);
        CHECK_EQ(result.selected_tests.size(), 1u);
        CHECK_EQ(build::run_tests(result, {}), 0);

        if (kind == TargetKind::Executable) {
            build::BuildRequest main_req;
            main_req.cwd = root;
            build::BuildResult built = build::run_build(main_req, {});
            CHECK_EQ(built.exit_code, 0);
            cpm::proc::Captured out = cpm::proc::capture({.argv = {(root / built.plan.main_output).string()}});
            CHECK_EQ(out.exit_code, 0);
            CHECK(out.out.starts_with("Hello, world!"));
        }
    }
}
