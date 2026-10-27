// Choosing the compiler (docs/spec/build.md §1.1, §1.4): requests, finding compilers, the order of choices, and a
// build with every compiler family the machine has.

#include <set>

#include "build/pipeline.hpp"
#include "core/env.hpp"
#include "core/fsx.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "manifest/config.hpp"
#include "setup/setup.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"
#include "toolchain/probe.hpp"
#include "toolchain/select.hpp"

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
using toolchain::Family;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).wstring().c_str(), value.empty() ? nullptr : fs::path(value).wstring().c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// Clears CXX for the test's duration: CI sets it, and it comes before the project and the machine.
struct WithoutCxx {
    std::string saved = env::get("CXX").value_or("");
    WithoutCxx() { set_env("CXX", ""); }
    ~WithoutCxx() { set_env("CXX", saved); }
};

std::string exe(const std::string& name) {
#ifdef _WIN32
    return name + ".exe";
#else
    return name;
#endif
}

toolchain::Toolchain fake(Family family, const std::string& version) {
    toolchain::Toolchain tc;
    tc.family = family;
    tc.version = version;
    return tc;
}

}  // namespace

TEST(requests_name_a_family_and_a_range) {
    auto gcc = toolchain::parse_request("gcc");
    CHECK((gcc && gcc->family == Family::Gcc && !gcc->range));
    auto clang = toolchain::parse_request("  clang  >=18 ");
    CHECK((clang && clang->family == Family::Clang && clang->range && clang->text == "clang  >=18"));
    CHECK(toolchain::parse_request("apple-clang 16")->family == Family::AppleClang);
    for (const char* compiler : {"g++-14", "clang++", "/usr/bin/g++", "C:/tools/g++.exe", "gcc++"}) {
        CHECK(!toolchain::parse_request(compiler).has_value());
    }
    CHECK_THROWS(toolchain::parse_request("gcc banana"), "invalid compiler version range \"banana\"");

    CHECK(toolchain::matches(fake(Family::Gcc, "14.2.0"), *toolchain::parse_request("gcc >=14")));
    CHECK(toolchain::matches(fake(Family::Gcc, "14.2.0"), *toolchain::parse_request("gcc 14")));
    CHECK(!toolchain::matches(fake(Family::Gcc, "13.2.0"), *toolchain::parse_request("gcc 14")));
    CHECK(!toolchain::matches(fake(Family::AppleClang, "17.0.0"), *toolchain::parse_request("clang")));  // Apple's is its own family
}

TEST(finds_compilers_by_name_once_each) {
    check::TempDir t;
    for (const char* name : {"g++", "g++-14", "clang++-18", "clang++", "c++", "x86_64-w64-mingw32-g++", "g++-abc", "g++-"}) {
        fixture::write(t.path() / "bin" / exe(name), "");
    }
    fixture::write(t.path() / "other" / exe("g++-13"), "");
    fixture::write(t.path() / "other" / exe("cl"), "");  // MSVC, by that name on Windows only
    fs::create_directories(t.path() / "bin" / exe("clang++-19"));  // a directory, not a program
    auto names = [](const std::vector<fs::path>& found) {
        std::vector<std::string> out;
        for (const auto& p : found) out.push_back(paths::to_utf8(p.parent_path().filename() / p.filename()));
        return out;
    };
    // clang++ before g++, unversioned before versioned; a directory listed twice counts once.
    std::vector<std::string> expected = {"bin/" + exe("clang++"), "bin/" + exe("clang++-18"), "bin/" + exe("g++"),
                                         "bin/" + exe("g++-14")};
#ifdef _WIN32
    expected.push_back("other/" + exe("cl"));
#endif
    expected.push_back("other/" + exe("g++-13"));
    CHECK_EQ(names(toolchain::find_compilers({t.path() / "bin", t.path() / "other", t.path() / "bin", t.path() / "missing"})),
             expected);
}

TEST(requests_pick_the_newest_usable_match) {
    auto found = [](Family f, const char* version, const char* path, std::string problem = "") {
        toolchain::Found x;
        x.cxx = path;
        x.toolchain = fake(f, version);
        x.problem = std::move(problem);
        return x;
    };
    std::vector<toolchain::Found> all = {found(Family::Gcc, "13.2.0", "/a/g++"), found(Family::Gcc, "14.1.0", "/a/g++-14"),
                                         found(Family::Gcc, "14.1.0", "/b/g++-14"), found(Family::Clang, "19.1.0", "/a/clang++",
                                                                                          "targets the MSVC ABI"),
                                         found(Family::Clang, "18.1.3", "/a/clang++-18")};
    CHECK_EQ(toolchain::best_match(all, *toolchain::parse_request("gcc"))->cxx, fs::path("/a/g++-14"));  // the first of equals
    CHECK_EQ(toolchain::best_match(all, *toolchain::parse_request("gcc 13"))->cxx, fs::path("/a/g++"));
    CHECK_EQ(toolchain::best_match(all, *toolchain::parse_request("clang"))->cxx, fs::path("/a/clang++-18"));  // 19 is unusable
    CHECK(toolchain::best_match(all, *toolchain::parse_request("clang >=19")) == nullptr);
    CHECK(toolchain::best_match(all, *toolchain::parse_request("apple-clang")) == nullptr);
}

TEST(projects_and_machines_state_their_choice) {
    auto parse = [](const std::string& toolchain) {
        return manifest::parse(R"({ "name": "app", "version": "1.0.0", "toolchain": )" + toolchain + " }", "cpm.jsonc", "cpm.jsonc");
    };
    CHECK_EQ(parse(R"("gcc >=14")").toolchain, (std::vector<std::string>{"gcc >=14"}));
    CHECK_EQ(parse(R"(["gcc >=14", "clang 18", "apple-clang", "msvc >=19.44"])").toolchain.size(), 4u);
    CHECK_THROWS(parse(R"("g++-14")"), "is not a compiler request");  // paths and names differ between machines
    CHECK_THROWS(parse(R"("clang >=banana")"), "invalid compiler version range");
    CHECK_THROWS(parse(R"([])"), "at least one");
    CHECK_THROWS(parse("14"), "a string or an array");

    check::TempDir home;
    manifest::set_user_config(home.path(), "toolchain", "clang >=18");
    manifest::set_user_config(home.path(), "cxx", "g++-14");
    auto cfg = manifest::load_user_config(home.path());
    CHECK(cfg.toolchain == std::optional<std::string>("clang >=18"));
    CHECK(cfg.cxx == std::optional<std::string>("g++-14"));
    manifest::set_user_config(home.path(), "cxx", std::nullopt);
    CHECK(!manifest::load_user_config(home.path()).cxx);
    CHECK(fsx::read_file(home.path() / "config.jsonc").starts_with("// CPM's user configuration"));
}

TEST(the_command_line_then_cxx_then_the_project_then_the_machine_choose) {
    WithoutCxx clear;
    check::TempDir t;
    fs::path home = t.path() / "home";
    // Whatever this machine finds on PATH: the other choices are checked against the same compiler.
    toolchain::Toolchain automatic = toolchain::find_toolchain({.home = home});
    CHECK_EQ(automatic.chosen_by, "found on PATH");
    std::string family(toolchain::to_string(automatic.family));
    std::string path = paths::to_utf8(automatic.cxx);

    CHECK_EQ(toolchain::find_toolchain({.config_toolchain = family, .home = home}).chosen_by, "from config.jsonc");
    CHECK_EQ(toolchain::find_toolchain({.config_cxx = path, .config_toolchain = "nonsense", .home = home}).cxx, automatic.cxx);
    toolchain::Toolchain project = toolchain::find_toolchain({.project = {family}, .config_cxx = path, .home = home});
    CHECK_EQ(project.chosen_by, "from cpm.jsonc");
    CHECK(project.family == automatic.family);
    toolchain::Toolchain command =
        toolchain::find_toolchain({.command = toolchain::Choice{path, "--cxx"}, .project = {family}, .home = home});
    CHECK_EQ(command.chosen_by, "from --cxx");
    CHECK_EQ(command.cxx, automatic.cxx);

    set_env("CXX", path);
    CHECK_EQ(toolchain::find_toolchain({.project = {family}, .home = home}).chosen_by, "from CXX");
    set_env("CXX", "");

    // A request nothing meets says what CPM found and how to get one.
    CHECK_THROWS(toolchain::find_toolchain({.project = {family + " >=999"}, .home = home}), "no compiler on this machine matches");
    CHECK_THROWS(toolchain::find_toolchain({.command = toolchain::Choice{"no-such-g++", "--toolchain"}, .home = home}),
                 "compiler 'no-such-g++' (from --toolchain) not found");
}

TEST(setup_installs_the_compiler_asked_for) {
    setup::Needs clang{.compiler = true, .compiler_family = "clang"};
    CHECK_EQ(setup::packages_for("apt-get", clang, "clang-19"), (std::vector<std::string>{"clang-19", "g++"}));
    CHECK_EQ(setup::packages_for("dnf", clang, ""), (std::vector<std::string>{"clang", "gcc-c++"}));
    CHECK_EQ(setup::packages_for("pacman", clang, ""), (std::vector<std::string>{"clang", "gcc"}));
    CHECK_EQ(setup::packages_for("apk", clang, ""), (std::vector<std::string>{"clang", "g++"}));
    auto offers = [](std::initializer_list<std::string_view> names) {
        std::vector<std::string_view> list(names);
        return [list](std::string_view p) { return std::find(list.begin(), list.end(), p) != list.end(); };
    };
    CHECK_EQ(setup::apt_clang_package(offers({"clang-17", "clang-18", "clang"})), "clang-18");
    CHECK_EQ(setup::apt_clang_package(offers({"clang-15", "clang"})), "clang");
}

TEST(builds_with_each_compiler_family_the_machine_has) {
    // CI's Linux runners have GCC and Clang (ROADMAP.md 0.4.4). Elsewhere this checks what there is.
    check::TempDir t;
    set_env("CPM_HOME", paths::to_utf8(t.path() / "home"));
    std::vector<std::string> families;
#ifdef __APPLE__
    const std::vector<const char*> wanted = {"apple-clang"};  // Homebrew's GCC and LLVM vary too much between machines
#elif defined(_WIN32)
    const std::vector<const char*> wanted = {"gcc", "clang", "msvc"};
#else
    const std::vector<const char*> wanted = {"gcc", "clang"};
#endif
    for (const char* family : wanted) {
        std::vector<toolchain::Found> found = toolchain::list_compilers(t.path() / "home");
        if (toolchain::best_match(found, *toolchain::parse_request(family))) families.push_back(family);
    }
    INFO("compiler families here: {}", str::join(families, ", "));
    CHECK(!families.empty());
#ifdef __linux__
    if (env::is_set("GITHUB_ACTIONS")) CHECK_EQ(families.size(), 2u);  // CI's Linux runners must test both
#endif

    fs::path app = t.path() / "app";
    fixture::write(app / "cpm.jsonc", R"({ "name": "app", "version": "1.0.0", "std": "c++17" })");
    fixture::write(app / "src" / "main.cpp",
                   "#include <cstdio>\nint main() {\n#if defined(_MSC_VER) && !defined(__clang__)\n  std::puts(\"msvc\");\n"
                   "#elif defined(__clang__)\n  std::puts(\"clang\");\n#else\n  std::puts(\"gcc\");\n#endif\n}\n");
    std::set<std::string> abis;
    for (const auto& family : families) {
        build::BuildRequest req;
        req.cwd = app;
        req.toolchain = toolchain::Choice{family, "--toolchain"};
        build::BuildResult built = build::run_build(req, manifest::UserConfig{});
        CHECK_EQ(built.exit_code, 0);
        CHECK_EQ(built.plan.toolchain.chosen_by, "from --toolchain");
        auto out = cpm::proc::capture({.argv = {paths::to_utf8(app / built.plan.main_output)}});
        CHECK(out.out.starts_with(family));
        abis.insert(build::load_project(app, "debug", req.toolchain, manifest::UserConfig{}).abi.dir());
    }
    CHECK_EQ(abis.size(), families.size());  // each compiler builds for its own ABI
}
