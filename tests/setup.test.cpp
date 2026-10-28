// cpm setup (docs/spec/cli.md): the decisions it makes, which need no network. Downloading and unpacking
// toolchains is exercised by running cpm setup itself.
#include <algorithm>
#include <set>

#include "core/fsx.hpp"
#include "manifest/config.hpp"
#include "setup/setup.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

namespace {

const std::string kToolchains = R"({
  "toolchainsVersion": 1,
  "platforms": {
    "x86_64-windows": {
      "compiler": { "name": "WinLibs GCC 16.2.0", "version": "16.2.0", "url": "https://example.org/winlibs.zip",
                    "sha256": "d5dbafc4a170e762ca6143151ec918fb9e2c72736fb14cd704abebc6bdd5276a", "size": 273613326,
                    "cxx": "mingw64/bin/g++.exe" },
      "ninja": { "version": "1.13.2", "url": "https://example.org/ninja-win.zip",
                 "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "size": 291570, "exe": "ninja.exe" }
    },
    "x86_64-linux": {
      "ninja": { "version": "1.13.2", "url": "https://example.org/ninja-linux.zip",
                 "sha256": "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6", "size": 134040, "exe": "ninja" }
    }
  }
})";

std::string with(std::string text, const std::string& from, const std::string& to) {
    text.replace(text.find(from), from.size(), to);
    return text;
}

}  // namespace

TEST(reads_the_tools_for_a_platform) {
    auto windows = setup::parse_toolchains(kToolchains, "x86_64-windows");
    CHECK(windows.compiler.has_value());
    CHECK_EQ(windows.compiler->name, "WinLibs GCC 16.2.0");
    CHECK_EQ(windows.compiler->program, "mingw64/bin/g++.exe");
    CHECK_EQ(windows.compiler->size, 273613326u);
    CHECK_EQ(windows.ninja->name, "Ninja 1.13.2");  // a name made up when the document has none
    auto linux_tools = setup::parse_toolchains(kToolchains, "x86_64-linux");
    CHECK(!linux_tools.compiler.has_value());
    CHECK(linux_tools.ninja.has_value());
    auto none = setup::parse_toolchains(kToolchains, "riscv64-linux");
    CHECK(!none.compiler.has_value());
    CHECK(!none.ninja.has_value());
}

TEST(refuses_toolchain_documents_it_cannot_trust) {
    CHECK_THROWS(setup::parse_toolchains(with(kToolchains, "\"toolchainsVersion\": 1", "\"toolchainsVersion\": 2"), "x86_64-windows"),
                 "does not understand");
    CHECK_THROWS(setup::parse_toolchains(with(kToolchains, "07fc8261b42b", "07FC8261B42B"), "x86_64-windows"), "invalid sha256");
    CHECK_THROWS(setup::parse_toolchains(with(kToolchains, "https://example.org/ninja-win.zip", "http://example.org/ninja-win.zip"), "x86_64-windows"),
                 "over https");
    CHECK_THROWS(setup::parse_toolchains(with(kToolchains, "mingw64/bin/g++.exe", "../g++.exe"), "x86_64-windows"), "invalid program path");
    CHECK_THROWS(setup::parse_toolchains(with(kToolchains, "\"exe\": \"ninja.exe\"", "\"program\": \"ninja.exe\""), "x86_64-windows"),
                 "has no \"exe\"");
    CHECK_THROWS(setup::parse_toolchains("{ not json", "x86_64-windows"), "toolchains.json is invalid");
}

TEST(knows_its_platform) {
    std::string key = setup::platform_key();
    std::set<std::string> known = {"x86_64-windows", "aarch64-windows", "x86_64-linux", "aarch64-linux", "x86_64-macos", "aarch64-macos"};
    CHECK(known.contains(key));
}

TEST(installs_what_is_missing_with_each_package_manager) {
    setup::Needs all{.compiler = true, .ninja = true, .git = true, .curl = true};
    CHECK_EQ(setup::packages_for("apt-get", all, "g++-14"),
             (std::vector<std::string>{"g++-14", "ninja-build", "git", "curl", "ca-certificates"}));
    CHECK_EQ(setup::packages_for("dnf", all, "g++"), (std::vector<std::string>{"gcc-c++", "ninja-build", "git", "curl"}));
    CHECK_EQ(setup::packages_for("pacman", all, "g++"), (std::vector<std::string>{"gcc", "ninja", "git", "curl"}));
    CHECK_EQ(setup::packages_for("zypper", all, "g++"), (std::vector<std::string>{"gcc-c++", "ninja", "git", "curl"}));
    CHECK_EQ(setup::packages_for("apk", all, "g++"), (std::vector<std::string>{"g++", "ninja-is-really-ninja", "git", "curl"}));
    CHECK_EQ(setup::packages_for("apt-get", {.ninja = true}, "g++"), (std::vector<std::string>{"ninja-build"}));
    CHECK_EQ(setup::packages_for("apt-get", {.curl = true}, "g++"), (std::vector<std::string>{"curl", "ca-certificates"}));
    CHECK_THROWS(setup::packages_for("brew", all, "g++"), "unknown package manager");

    CHECK_EQ(setup::install_command("apt-get", {"g++-14"}, false),
             (std::vector<std::string>{"sudo", "apt-get", "install", "-y", "--no-install-recommends", "g++-14"}));
    CHECK_EQ(setup::install_command("pacman", {"gcc"}, true), (std::vector<std::string>{"pacman", "-S", "--needed", "--noconfirm", "gcc"}));
    CHECK_EQ(setup::install_command("apk", {"g++"}, true), (std::vector<std::string>{"apk", "add", "g++"}));
}

TEST(picks_the_newest_gcc_apt_offers) {
    auto offers = [](std::set<std::string> packages) {
        return [packages](std::string_view p) { return packages.contains(std::string(p)); };
    };
    CHECK_EQ(setup::apt_compiler_package(offers({"g++-13", "g++-14", "g++"})), "g++-14");
    CHECK_EQ(setup::apt_compiler_package(offers({"g++-13", "g++"})), "g++-13");
    CHECK_EQ(setup::apt_compiler_package(offers({"g++-12", "g++"})), "g++");  // too old: plain g++, and setup checks the result
}

TEST(apt_policy_counts_only_the_package_asked_for) {
    const char* exact =
        "g++-14:\n"
        "  Installed: (none)\n"
        "  Candidate: 14.2.0-4ubuntu2~24.04\n"
        "  Version table:\n"
        "     14.2.0-4ubuntu2~24.04 500\n"
        "        500 http://archive.ubuntu.com/ubuntu noble-updates/universe amd64 Packages\n";
    CHECK(setup::apt_policy_offers(exact, "g++-14"));
    CHECK(!setup::apt_policy_offers(exact, "g++-1"));

    // Ubuntu 24.04 has no g++-15, so apt reads the name as a regular expression and lists what matches it.
    const char* regex =
        "libclang-15-dev:\n"
        "  Installed: (none)\n"
        "  Candidate: 1:15.0.7-14build3\n"
        "python3-clang-15:\n"
        "  Installed: (none)\n"
        "  Candidate: 1:15.0.7-14build3\n";
    CHECK(!setup::apt_policy_offers(regex, "g++-15"));

    CHECK(!setup::apt_policy_offers("g++-15:\r\n  Installed: (none)\r\n  Candidate: (none)\r\n", "g++-15"));
    CHECK(!setup::apt_policy_offers("", "g++-15"));
}

TEST(writes_the_configuration_without_losing_comments) {
    check::TempDir home;
    setup::set_config(home.path(), "ninja", "C:/tools/ninja.exe");
    CHECK_EQ(manifest::load_user_config(home.path()).ninja.value_or(""), "C:/tools/ninja.exe");

    fsx::write_file_atomic(home.path() / "config.jsonc",
                           "// mine\n{\n  // the registry I use\n  \"registries\": { \"default\": \"https://cpm-registry.com/\" }\n}\n");
    setup::set_config(home.path(), "cxx", "C:/tools/g++.exe");
    setup::set_config(home.path(), "cxx", "C:/tools/winlibs/g++.exe");
    std::string text = fsx::read_file(home.path() / "config.jsonc");
    CHECK(text.contains("// the registry I use"));
    auto config = manifest::load_user_config(home.path());
    CHECK_EQ(config.cxx.value_or(""), "C:/tools/winlibs/g++.exe");
    CHECK_EQ(config.registries.at("default").url, "https://cpm-registry.com/");
}

TEST(msvc_uses_visual_studio_or_installs_build_tools) {
    // cpm setup --compiler msvc (cli.md): an installed Visual Studio is used as it is; without one, winget installs
    // Build Tools; without winget, CPM can only say where to get them.
    CHECK(setup::msvc_action(true, true) == setup::MsvcAction::Use);
    CHECK(setup::msvc_action(true, false) == setup::MsvcAction::Use);
    CHECK(setup::msvc_action(false, true) == setup::MsvcAction::InstallBuildTools);
    CHECK(setup::msvc_action(false, false) == setup::MsvcAction::Unavailable);

    std::vector<std::string> command = setup::build_tools_command();
    auto has = [&](const std::string& arg) { return std::find(command.begin(), command.end(), arg) != command.end(); };
    CHECK_EQ(command.front(), "winget");
    CHECK(has("Microsoft.VisualStudio.BuildTools"));  // Build Tools 2026
    CHECK(has("--exact"));
    CHECK(has("--accept-package-agreements"));       // the user said yes, which accepts the license
    CHECK(command.back().contains("Microsoft.VisualStudio.Workload.VCTools"));
    CHECK(command.back().contains("--includeRecommended"));  // with the CMake tools, whose Ninja CPM uses
    CHECK(command.back().contains("--passive"));
}
