#include "setup/setup.hpp"

#include <algorithm>
#include <format>
#include <random>

#include "build/runner.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "core/system_tar.hpp"
#include "core/term.hpp"
#include "install/default_registry.hpp"
#include "install/fetch.hpp"
#include "json/edit.hpp"
#include "json/parse.hpp"
#include "manifest/config.hpp"
#include "toolchain/probe.hpp"
#include "toolchain/select.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cpm::setup {

namespace {

// The oldest compilers CPM supports (README.md).
constexpr int kMinGcc = 13;
constexpr int kMinClang = 17;
constexpr int kMinAppleClang = 15;

int leading_number(std::string_view text) {
    int n = 0;
    std::size_t i = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') n = n * 10 + (text[i++] - '0');
    return i == 0 ? -1 : n;
}

std::string size_text(std::uint64_t bytes) {
    if (bytes >= 1024 * 1024) return std::format("{} MB", (bytes + 512 * 1024) / (1024 * 1024));
    return std::format("{} KB", (bytes + 512) / 1024);
}

std::string random_suffix() {
    std::random_device rd;
    return std::format("{:08x}", rd());
}

// ---- what the machine has

struct CompilerState {
    std::optional<toolchain::Toolchain> usable;
    std::string problem;  // why there is no usable compiler
};

CompilerState inspect_compiler(const fs::path& home) {
    manifest::UserConfig config = manifest::load_user_config(home);
    CompilerState s;
    try {
        toolchain::Toolchain tc =
            toolchain::find_toolchain({.config_cxx = config.cxx, .config_toolchain = config.toolchain, .home = home});
        int need = tc.family == toolchain::Family::Gcc          ? kMinGcc
                   : tc.family == toolchain::Family::AppleClang ? kMinAppleClang
                                                                : kMinClang;
        if (leading_number(tc.version) < need) {
            s.problem = std::format("{} at {} is too old: CPM needs {} {} or newer", tc.description(),
                                    paths::to_utf8(tc.cxx), toolchain::to_string(tc.family), need);
            return s;
        }
        toolchain::std_flag(tc, "c++23", home);  // throws when the compiler cannot do C++23
        s.usable = tc;
    } catch (const Error& e) {
        s.problem = e.what();
    }
    return s;
}

struct NinjaState {
    std::optional<fs::path> usable;
    std::string version;
    std::string problem;
};

NinjaState inspect_ninja(const fs::path& home) {
    manifest::UserConfig config = manifest::load_user_config(home);
    NinjaState s;
    try {
        fs::path ninja = build::find_ninja(config.ninja, home);
        proc::Captured r = proc::capture({.argv = {paths::to_utf8(ninja), "--version"}});
        std::string version(str::trim(r.out));
        auto parts = str::split(version, '.');
        int major = parts.empty() ? -1 : leading_number(parts[0]);
        int minor = parts.size() < 2 ? -1 : leading_number(parts[1]);
        if (r.exit_code != 0 || major < 1 || (major == 1 && minor < 10)) {
            s.problem = std::format("Ninja at {} is {}: CPM needs 1.10 or newer", paths::to_utf8(ninja), version.empty() ? "not working" : version);
            return s;
        }
        s.usable = ninja;
        s.version = version;
    } catch (const Error& e) {
        s.problem = e.what();
    }
    return s;
}

struct State {
    CompilerState compiler;
    NinjaState ninja;
    std::optional<fs::path> git, curl, tar;
};

State inspect(const fs::path& home) {
    State s;
    s.compiler = inspect_compiler(home);
    s.ninja = inspect_ninja(home);
    s.git = proc::find_program("git");
    s.curl = proc::find_program("curl");
    s.tar = system_tar::locate();  // the tar that extracts packages, which on Windows need not be the first on PATH
    return s;
}

void report(const State& s) {
    const std::string missing = log::yellow("missing");
    if (s.compiler.usable) {
        log::status("Compiler", std::format("{} ({})", s.compiler.usable->description(), paths::to_utf8(s.compiler.usable->cxx)));
    } else {
        log::status("Compiler", std::format("{}: {}", missing, s.compiler.problem));
    }
    if (s.ninja.usable) log::status("Ninja", std::format("{} ({})", s.ninja.version, paths::to_utf8(*s.ninja.usable)));
    else log::status("Ninja", std::format("{}: {}", missing, s.ninja.problem));
    for (const auto& [name, found] : {std::pair{"git", s.git}, std::pair{"curl", s.curl}, std::pair{"tar", s.tar}}) {
        log::status(name, found ? paths::to_utf8(*found) : missing);
    }
}

bool ready(const State& s) { return s.compiler.usable && s.ninja.usable && s.curl && s.tar; }

// ---- doing it

bool confirm(const Options& options, const std::vector<std::string>& actions) {
    log::info("cpm setup will:");
    for (const auto& a : actions) log::info("  - " + a);
    if (options.yes) return true;
    log::err("Continue? [Y/n] ");
    auto answer = term::read_line();
    if (!answer) {
        log::info("");
        log::info("No answer (no terminal?); run cpm setup --yes to go ahead without asking.");
        return false;
    }
    std::string a = str::to_lower(std::string(str::trim(*answer)));
    return a.empty() || a == "y" || a == "yes";
}

[[maybe_unused]] int run_command(const std::vector<std::string>& argv) {
    log::status("Running", proc::command_line(argv));
    // Inherits the terminal, so sudo and the package manager can ask their own questions.
    return proc::run({.argv = argv}, {.stdout_mode = proc::Stdout::ToStderr});
}

void download(const std::string& url, const fs::path& dest) {
    if (install::url::is_file(url)) {
        if (!install::download(url, dest, false)) throw Error(std::format("{} does not exist", url));
        return;
    }
    auto curl = proc::find_program("curl");
    if (!curl) throw Error("downloading needs curl, which was not found on PATH", "install curl (https://curl.se)");
    fsx::create_dirs(dest.parent_path());
    std::vector<std::string> argv = {paths::to_utf8(*curl), "-fL", "--retry", "2", "--proto", "=https",
                                     "-o", paths::to_utf8(dest), url};
    if (term::is_terminal(2)) {
        argv.insert(argv.begin() + 1, "--progress-bar");
        if (proc::run({.argv = argv}, {.stdout_mode = proc::Stdout::ToStderr}) != 0) throw Error(std::format("cannot download {}", url));
    } else {
        argv.insert(argv.begin() + 1, "-sS");
        proc::Captured r = proc::capture({.argv = argv});
        if (r.exit_code != 0) throw Error(std::format("cannot download {}: {}", url, str::trim(r.err)));
    }
}

void unpack_zip(const fs::path& archive, const fs::path& dest) {
#if defined(_WIN32) || defined(__APPLE__)
    // Windows 10+ and macOS ship bsdtar, which reads zip files.
    system_tar::extract({.archive = archive, .dest = dest}, paths::to_utf8(archive.filename()));
#else
    fsx::create_dirs(dest);
    std::vector<std::string> argv;
    if (auto unzip = proc::find_program("unzip")) {
        argv = {paths::to_utf8(*unzip), "-q", "-o", paths::to_utf8(archive), "-d", paths::to_utf8(dest)};
    } else if (auto python = proc::find_program("python3")) {
        argv = {paths::to_utf8(*python), "-c", "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])",
                paths::to_utf8(archive), paths::to_utf8(dest)};
    } else {
        throw Error("unpacking a .zip needs unzip or python3", "install unzip with your package manager");
    }
    proc::Captured r = proc::capture({.argv = argv});
    if (r.exit_code != 0) throw Error(std::format("cannot unpack {}: {}", paths::to_utf8(archive.filename()), str::trim(r.err)));
#endif
}

// Downloads, verifies and unpacks a tool into $CPM_HOME/tools/<dir>-<version>/ (unless it is already there) and
// returns its program.
fs::path install_tool(const Tool& tool, const std::string& dir, const fs::path& home) {
    fs::path dest = home / "tools" / std::format("{}-{}", dir, tool.version);
    fs::path program = dest / paths::from_utf8(tool.program);
    std::error_code ec;
    if (fs::is_regular_file(program, ec)) {
        log::status("Found", std::format("{} in {}", tool.name, paths::to_utf8(dest)));
        return program;
    }
    fs::path tmp = home / "tmp" / ("setup-" + random_suffix());
    fsx::create_dirs(tmp);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            try {
                fsx::remove_tree(dir);
            } catch (...) {
            }
        }
    } cleanup{tmp};

    log::status("Downloading", std::format("{} ({})", tool.name, size_text(tool.size)));
    fs::path archive = tmp / "download.zip";
    download(tool.url, archive);
    std::string actual = install::sha256_file(archive);
    if (actual != tool.sha256) {
        throw Error(std::format("the download of {} is corrupt (expected sha256:{}, got sha256:{})", tool.name, tool.sha256, actual),
                    "run cpm setup again; if it keeps failing, report it at https://github.com/meirborowski/cpm-registry");
    }
    log::status("Unpacking", tool.size > 50 * 1024 * 1024 ? tool.name + " (this takes a minute or two)" : tool.name);
    fs::path unpacked = tmp / "unpacked";
    unpack_zip(archive, unpacked);
    if (!fs::is_regular_file(unpacked / paths::from_utf8(tool.program), ec)) {
        throw Error(std::format("{} does not contain {}", tool.name, tool.program));
    }
    if (fs::exists(dest, ec)) fsx::remove_tree(dest);
    fsx::create_dirs(dest.parent_path());
    fsx::rename_replace(unpacked, dest);
#ifndef _WIN32
    fs::permissions(program, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
    log::status("Installed", std::format("{} in {}", tool.name, paths::to_utf8(dest)));
    return program;
}

PlatformTools fetch_tools(const fs::path& home) {
    std::string url = env::get("CPM_TOOLCHAINS_URL").value_or("");
    if (url.empty()) url = std::string(install::kBuiltinRegistry) + "toolchains.json";
    // Plain http is accepted only for a registry running on this machine.
    bool local = url.starts_with("http://127.0.0.1") || url.starts_with("http://localhost");
    fs::path file = home / "tmp" / std::format("toolchains-{}.json", random_suffix());
    if (!install::download(url, file, local)) throw Error(std::format("{} was not found", url));
    std::string text = fsx::read_file(file);
    std::error_code ec;
    fs::remove(file, ec);
    return parse_toolchains(text, platform_key());
}

const Tool& offered(const std::optional<Tool>& tool, std::string_view what) {
    if (!tool) {
        throw Error(std::format("CPM does not offer {} for {} yet", what, platform_key()),
                    "install one yourself; README.md lists what CPM needs");
    }
    return *tool;
}

std::string tools_dir(const fs::path& home) { return paths::to_utf8(home / "tools"); }

#ifndef _WIN32
[[maybe_unused]] bool is_root() { return geteuid() == 0; }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
bool apt_has(std::string_view package) {
    proc::Captured r = proc::capture({.argv = {"apt-cache", "policy", std::string(package)}});
    return r.exit_code == 0 && apt_policy_offers(r.out, package);
}
#endif

}  // namespace

// ---- pure helpers

std::string platform_key() {
#if defined(__x86_64__) || defined(_M_X64)
    std::string arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    std::string arch = "aarch64";
#else
    std::string arch = "unknown";
#endif
#if defined(_WIN32)
    return arch + "-windows";
#elif defined(__APPLE__)
    return arch + "-macos";
#elif defined(__linux__)
    return arch + "-linux";
#else
    return arch + "-unknown";
#endif
}

PlatformTools parse_toolchains(std::string_view text, const std::string& platform) {
    json::Value doc;
    try {
        doc = json::parse(text, json::kStrict);
    } catch (const Error& e) {
        throw Error(std::format("toolchains.json is invalid: {}", e.what()));
    }
    const json::Value* version = doc.find("toolchainsVersion");
    if (!version || !version->is_int() || version->as_int() != 1) {
        throw Error("toolchains.json has a format this CPM does not understand", "upgrade CPM");
    }
    PlatformTools out;
    const json::Value* platforms = doc.find("platforms");
    const json::Value* entry = platforms ? platforms->find(platform) : nullptr;
    if (!entry) return out;

    auto tool = [&](const char* key, const char* program_key, std::string_view label) -> std::optional<Tool> {
        const json::Value* t = entry->find(key);
        if (!t) return std::nullopt;
        auto field = [&](const char* name) {
            const json::Value* v = t->find(name);
            if (!v || !v->is_string() || v->as_string().empty())
                throw Error(std::format("toolchains.json: {} {} has no \"{}\"", platform, key, name));
            return v->as_string();
        };
        Tool r;
        r.version = field("version");
        r.url = field("url");
        r.sha256 = field("sha256");
        r.program = field(program_key);
        const json::Value* name = t->find("name");
        r.name = name && name->is_string() ? name->as_string() : std::format("{} {}", label, r.version);
        if (const json::Value* size = t->find("size"); size && size->is_int() && size->as_int() > 0)
            r.size = static_cast<std::uint64_t>(size->as_int());
        bool hex = r.sha256.size() == 64 && std::all_of(r.sha256.begin(), r.sha256.end(), [](char c) {
                       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                   });
        if (!hex) throw Error(std::format("toolchains.json: {} {} has an invalid sha256", platform, key));
        if (!r.url.starts_with("https://") && !install::url::is_file(r.url))
            throw Error(std::format("toolchains.json: {} {} must be downloaded over https", platform, key));
        auto segments = str::split(r.program, '/');
        if (r.program.starts_with('/') || std::any_of(segments.begin(), segments.end(), [](const std::string& s) { return s.empty() || s == ".."; }))
            throw Error(std::format("toolchains.json: {} {} has an invalid program path", platform, key));
        return r;
    };
    out.compiler = tool("compiler", "cxx", "Compiler");
    out.ninja = tool("ninja", "exe", "Ninja");
    return out;
}

const std::vector<std::string>& package_managers() {
    static const std::vector<std::string> managers = {"apt-get", "dnf", "pacman", "zypper", "apk"};
    return managers;
}

std::vector<std::string> packages_for(const std::string& manager, const Needs& needs, const std::string& compiler_package) {
    struct Names {
        std::string compiler, ninja;
    };
    Names n;
    if (manager == "apt-get") n = {compiler_package, "ninja-build"};
    else if (manager == "dnf") n = {"gcc-c++", "ninja-build"};
    else if (manager == "pacman") n = {"gcc", "ninja"};
    else if (manager == "zypper") n = {"gcc-c++", "ninja"};
    else if (manager == "apk") n = {"g++", "ninja-is-really-ninja"};  // "ninja" is samurai on Alpine
    else throw Error(std::format("unknown package manager '{}'", manager));
    std::vector<std::string> packages;
    if (needs.compiler && needs.compiler_family == "clang") {
        packages.push_back(manager == "apt-get" ? compiler_package : "clang");
        packages.push_back(manager == "apt-get" ? "g++" : n.compiler);  // GCC's standard library, which Clang uses here
    } else if (needs.compiler) {
        packages.push_back(n.compiler);
    }
    if (needs.ninja) packages.push_back(n.ninja);
    if (needs.git) packages.push_back("git");
    if (needs.curl) packages.push_back("curl");
    // apt-get runs with --no-install-recommends, which leaves out the certificates https needs.
    if (manager == "apt-get" && (needs.git || needs.curl)) packages.push_back("ca-certificates");
    return packages;
}

std::vector<std::string> install_command(const std::string& manager, const std::vector<std::string>& packages, bool root) {
    std::vector<std::string> cmd;
    if (!root) cmd.push_back("sudo");
    if (manager == "apt-get") cmd.insert(cmd.end(), {"apt-get", "install", "-y", "--no-install-recommends"});
    else if (manager == "dnf") cmd.insert(cmd.end(), {"dnf", "install", "-y"});
    else if (manager == "pacman") cmd.insert(cmd.end(), {"pacman", "-S", "--needed", "--noconfirm"});
    else if (manager == "zypper") cmd.insert(cmd.end(), {"zypper", "--non-interactive", "install"});
    else if (manager == "apk") cmd.insert(cmd.end(), {"apk", "add"});
    else throw Error(std::format("unknown package manager '{}'", manager));
    cmd.insert(cmd.end(), packages.begin(), packages.end());
    return cmd;
}

std::string apt_compiler_package(const std::function<bool(std::string_view)>& available) {
    for (const char* p : {"g++-15", "g++-14", "g++-13"}) {
        if (available(p)) return p;
    }
    return "g++";
}

std::string apt_clang_package(const std::function<bool(std::string_view)>& available) {
    for (const char* p : {"clang-20", "clang-19", "clang-18", "clang-17"}) {
        if (available(p)) return p;
    }
    return "clang";
}

bool apt_policy_offers(std::string_view policy, std::string_view package) {
    bool in_entry = false;
    for (std::string_view line : str::split_lines(policy)) {
        if (line.empty()) continue;
        if (line.front() != ' ') {  // "<name>:" starts each package's entry
            in_entry = line.size() == package.size() + 1 && line.starts_with(package) && line.back() == ':';
            continue;
        }
        std::string_view field = str::trim(line);
        if (in_entry && field.starts_with("Candidate:")) return str::trim(field.substr(10)) != "(none)";
    }
    return false;
}

void set_config(const fs::path& home, const std::string& key, const std::string& value) {
    manifest::set_user_config(home, key, value);
}

// After `cpm setup --compiler`: the newest compiler of that family CPM now finds becomes the machine's choice.
void choose_installed(const fs::path& home, toolchain::Family family) {
    std::vector<toolchain::Found> found = toolchain::list_compilers(home);
    toolchain::Request request{family, std::nullopt, std::string(toolchain::to_string(family))};
    const toolchain::Found* best = toolchain::best_match(found, request);
    if (best == nullptr) {
        throw Error(std::format("{} was installed, but CPM doesn't find it", toolchain::to_string(family)),
                    "open a new terminal so PATH includes it, then run cpm toolchain use " + std::string(toolchain::to_string(family)));
    }
    set_config(home, "cxx", paths::to_utf8(best->cxx));
    manifest::set_user_config(home, "toolchain", std::nullopt);
}

// ---- the command

int run(const Options& options, const fs::path& home) {
    log::status("Checking", std::format("what CPM needs on this machine ({})", platform_key()));
    State state = inspect(home);
    report(state);

    std::vector<std::string> actions;
    std::vector<std::string> notes;
    std::function<void()> act = [] {};
    bool need_compiler = !state.compiler.usable || options.compiler.has_value();
    bool need_ninja = !state.ninja.usable;
    bool note_curl = !state.curl;  // unless setup installs it
    if (!state.tar) notes.push_back("tar is needed to unpack packages; install it with your system's package manager");
    if (env::get("CXX").value_or("") != "" && !state.compiler.usable) {
        notes.push_back("the CXX environment variable chooses the compiler, so CPM uses it even after setup; unset it to let CPM choose");
    }

#if defined(_WIN32)
    if (options.compiler == "clang") {
        throw Error("cpm setup can't install Clang on Windows yet",
                    "install llvm-mingw (github.com/mstorsjo/llvm-mingw) or MSYS2's mingw-w64-ucrt-x86_64-clang, then run "
                    "cpm toolchain use clang");
    }
    if (!state.git) notes.push_back("git is only needed for git dependencies; get it with: winget install --id Git.Git -e");
    bool get_compiler = need_compiler || options.managed;
    bool get_ninja = need_ninja || options.managed;
    std::optional<PlatformTools> tools;
    if ((get_compiler || get_ninja) && !options.check) {
        tools = fetch_tools(home);
        if (get_compiler) {
            const Tool& t = offered(tools->compiler, "a compiler");
            // GCC reaches its headers through paths like <dir>/bin/../lib/gcc/<target>/<version>/../../../../include/
            // c++/<version>/<target>/bits/..., about 150 characters beyond its directory, and Windows limits paths to
            // 260: a deep CPM_HOME breaks every #include.
            if (std::string dir = paths::to_utf8(home / "tools" / std::format("winlibs-{}", t.version)); dir.size() > 100) {
                throw Error(std::format("{} is too deep a folder for GCC: Windows limits paths to 260 characters", dir),
                            "set CPM_HOME to a shorter folder, such as C:\\cpm, and run cpm setup again");
            }
            actions.push_back(std::format("download {} ({}) into {}", t.name, size_text(t.size), tools_dir(home)));
        }
        if (get_ninja) {
            const Tool& t = offered(tools->ninja, "Ninja");
            actions.push_back(std::format("download {} ({}) into {}", t.name, size_text(t.size), tools_dir(home)));
        }
        actions.push_back("point $CPM_HOME/config.jsonc at them");
        act = [&] {
            if (get_compiler) {
                set_config(home, "cxx", paths::to_utf8(install_tool(*tools->compiler, "winlibs", home)));
                manifest::set_user_config(home, "toolchain", std::nullopt);
            }
            if (get_ninja) set_config(home, "ninja", paths::to_utf8(install_tool(*tools->ninja, "ninja", home)));
        };
    }
#elif defined(__APPLE__)
    bool get_ninja = need_ninja || options.managed;
    bool clt_missing = proc::capture({.argv = {"xcode-select", "-p"}}).exit_code != 0;
    bool rerun = false;
    std::optional<std::string> formula;  // --compiler: GCC or LLVM's Clang from Homebrew
    if (options.compiler) {
        if (!proc::find_program("brew")) {
            throw Error(std::format("installing {} on macOS needs Homebrew", *options.compiler),
                        "install Homebrew from https://brew.sh, or use Apple's Clang: cpm setup without --compiler");
        }
        formula = *options.compiler == "gcc" ? "gcc" : "llvm";
        actions.push_back(std::format("install {} with Homebrew (brew install {})", *options.compiler == "gcc" ? "GCC" : "LLVM's Clang", *formula));
        need_compiler = false;  // Apple's Command Line Tools are not what was asked for
    }
    if (need_compiler) {
        if (clt_missing) {
            actions.push_back("install Apple's Command Line Tools (xcode-select --install; a dialog asks you to confirm)");
            rerun = true;
        } else {
            notes.push_back("update Apple's Command Line Tools (softwareupdate --list, or a newer Xcode) for a compiler CPM supports");
        }
    }
    std::optional<PlatformTools> tools;
    if (get_ninja && !options.check) {
        tools = fetch_tools(home);
        const Tool& t = offered(tools->ninja, "Ninja");
        actions.push_back(std::format("download {} ({}) into {}", t.name, size_text(t.size), tools_dir(home)));
    }
    act = [&] {
        if (formula) {
            if (run_command({"brew", "install", *formula}) != 0) throw Error(std::format("brew install {} failed", *formula));
            choose_installed(home, *options.compiler == "gcc" ? toolchain::Family::Gcc : toolchain::Family::Clang);
        }
        if (need_compiler && clt_missing) run_command({"xcode-select", "--install"});
        if (get_ninja) set_config(home, "ninja", paths::to_utf8(install_tool(*tools->ninja, "ninja", home)));
        if (rerun) notes.push_back("finish installing the Command Line Tools in the dialog, then run cpm setup again");
    };
#else
    Needs needs{.compiler = need_compiler,
                .compiler_family = options.compiler.value_or("gcc"),
                .ninja = need_ninja && !options.managed,
                .git = !state.git,
                .curl = !state.curl};
    std::string manager;
    for (const auto& m : package_managers()) {
        if (proc::find_program(m)) {
            manager = m;
            break;
        }
    }
    bool use_packages = !manager.empty() && (needs.compiler || needs.ninja || needs.git || needs.curl);
    if (use_packages && needs.curl) note_curl = false;
    bool download_ninja = options.managed || (need_ninja && manager.empty());
    bool root = is_root();
    if (!root && use_packages && !proc::find_program("sudo")) {
        throw Error("installing packages needs root, and sudo was not found", "run cpm setup as root");
    }
    if (manager.empty() && need_compiler) notes.push_back("install GCC 13 or newer (or Clang 17 or newer) with your system's tools");
    std::optional<PlatformTools> tools;
    if (use_packages) {
        std::string list;
        const char* compiler_text = needs.compiler_family == "clang" ? "Clang (with g++, for its standard library)" : "a GCC compiler";
        for (auto [what, needed] : {std::pair{compiler_text, needs.compiler}, std::pair{"Ninja", needs.ninja}, std::pair{"git", needs.git},
                                    std::pair{"curl", needs.curl}}) {
            if (needed) list += std::string(list.empty() ? "" : ", ") + what;
        }
        actions.push_back(std::format("install {} with {}{}", list, manager, root ? "" : " (through sudo, which may ask for your password)"));
    }
    if (download_ninja && !options.check) {
        tools = fetch_tools(home);
        const Tool& t = offered(tools->ninja, "Ninja");
        actions.push_back(std::format("download {} ({}) into {}", t.name, size_text(t.size), tools_dir(home)));
    }
    act = [&] {
        if (use_packages) {
            std::string compiler_package = "g++";
            if (manager == "apt-get") {
                std::vector<std::string> update = {"apt-get", "update"};
                if (!root) update.insert(update.begin(), "sudo");
                if (run_command(update) != 0) throw Error("apt-get update failed");
                if (needs.compiler) {
                    compiler_package = needs.compiler_family == "clang" ? apt_clang_package(apt_has) : apt_compiler_package(apt_has);
                }
            }
            if (run_command(install_command(manager, packages_for(manager, needs, compiler_package), root)) != 0) {
                throw Error(std::format("{} could not install the packages", manager));
            }
            if (options.compiler) {
                choose_installed(home, needs.compiler_family == "clang" ? toolchain::Family::Clang : toolchain::Family::Gcc);
            } else if (needs.compiler && compiler_package != "g++") {
                // A versioned GCC (g++-14) is not what `g++` runs, so CPM is told to use it.
                set_config(home, "cxx", compiler_package);
            }
        }
        if (download_ninja) set_config(home, "ninja", paths::to_utf8(install_tool(*tools->ninja, "ninja", home)));
    };
#endif
    if (note_curl) notes.push_back("curl is needed to download packages; install it with your system's package manager");

    if (actions.empty()) {
        for (const auto& n : notes) log::info("note: " + n);
        if (ready(state)) {
            log::status("Ready", "this machine has everything CPM needs");
            return 0;
        }
        log::error("something CPM needs is missing", "see the notes above");
        return 1;
    }
    if (options.check) {
        log::info("cpm setup would:");
        for (const auto& a : actions) log::info("  - " + a);
        for (const auto& n : notes) log::info("note: " + n);
        return 1;
    }
    if (!confirm(options, actions)) {
        log::info("Nothing was changed.");
        return 1;
    }
    act();

    log::status("Checking", "the result");
    State after = inspect(home);
    report(after);
    for (const auto& n : notes) log::info("note: " + n);
    if (!ready(after)) {
        log::error("CPM is not ready yet", "see above for what is still missing");
        return 1;
    }
    log::status("Ready", "try: cpm init -y hello && cd hello && cpm run");
    return 0;
}

}  // namespace cpm::setup
