#include "toolchain/msvc.hpp"

#include <array>
#include <format>
#include <vector>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "toolchain/probe_cache.hpp"
#include "toolchain/visual_studio.hpp"

namespace cpm::toolchain::msvc {

namespace {

// The macros the probe asks for. Each is printed after a marker of its own, cpm_probe<macro>, so that its line can be
// told from the expanded headers (abi.md §4).
constexpr std::array<std::string_view, 5> kProbed = {"_MSC_FULL_VER", "_MSVC_STL_VERSION", "_M_X64", "_M_ARM64", "_M_IX86"};
constexpr std::string_view kMarker = "cpm_probe";

std::string tail(std::string_view text, std::size_t lines) {
    auto all = str::split_lines(text);
    std::size_t start = all.size() > lines ? all.size() - lines : 0;
    return str::join(std::vector<std::string>(all.begin() + static_cast<std::ptrdiff_t>(start), all.end()), "\n");
}

fs::path scratch_file(const fs::path& home, const char* name, std::string_view content) {
    fs::path p = home / "tmp" / name;
    fsx::write_if_changed(p, content);
    return p;
}

proc::Captured run_cl(const Toolchain& tc, std::vector<std::string> args, const fs::path& cwd) {
    args.insert(args.begin(), paths::to_utf8(tc.cxx));
    return proc::capture({.argv = args, .cwd = cwd, .env_set = process_environment(tc)});
}

// The probe's source: the headers that define the library's macros, then one marker line per macro.
std::string probe_source() {
    std::string text = "#include <version>\n#include <cstdlib>\n";
    for (std::string_view macro : kProbed) text += std::format("{}{}={}\n", kMarker, macro, macro);
    return text;
}

int number(std::string_view text) {
    int n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return text.empty() ? -1 : n;
}

}  // namespace

bool is_cl(const fs::path& compiler) { return str::to_lower(paths::to_utf8(compiler.stem())) == "cl"; }

std::map<std::string, std::string> parse_probe(std::string_view preprocessed) {
    std::map<std::string, std::string> macros;
    for (const auto& line : str::split_lines(preprocessed)) {
        std::string_view l = str::trim(line);
        if (!l.starts_with(kMarker)) continue;
        l.remove_prefix(kMarker.size());
        auto eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        std::string name(str::trim(l.substr(0, eq)));
        std::string value(str::trim(l.substr(eq + 1)));
        if (!value.empty() && value != name) macros[name] = value;  // an undefined macro stays as its own name
    }
    return macros;
}

std::string display_version(std::string_view full_ver) {
    // _MSC_FULL_VER has 9 digits since Visual Studio 2015: 19 51 36260.
    if (full_ver.size() != 9 || number(full_ver) < 0) return std::string(full_ver);
    return std::format("{}.{}.{}", number(full_ver.substr(0, 2)), number(full_ver.substr(2, 2)), number(full_ver.substr(4)));
}

std::string full_version(std::string_view version) {
    auto parts = str::split(version, '.');
    if (parts.size() != 3) return std::string(version);
    return std::format("{}{:02}{:05}", number(parts[0]), number(parts[1]), number(parts[2]));
}

Toolchain toolchain_from_probe(const std::map<std::string, std::string>& macros, const fs::path& cl) {
    auto full = macros.find("_MSC_FULL_VER");
    if (full == macros.end()) {
        throw Error(std::format("'{}' does not look like MSVC: it doesn't define _MSC_FULL_VER", paths::to_utf8(cl)));
    }
    Toolchain tc;
    tc.family = Family::Msvc;
    tc.cxx = cl;
    tc.version = display_version(full->second);
    tc.os = Os::Windows;
    if (macros.contains("_M_X64")) tc.arch = "x86_64";
    else if (macros.contains("_M_ARM64")) tc.arch = "aarch64";
    else if (macros.contains("_M_IX86")) tc.arch = "x86";
    else tc.arch = "unknown";
    tc.target = tc.arch + "-pc-windows-msvc";
    tc.stdlib = "msvc-stl";
    auto stl = macros.find("_MSVC_STL_VERSION");
    tc.stdlib_abi = stl == macros.end() ? "unknown" : stl->second;
    tc.crt = "ucrt";
    return tc;
}

std::optional<std::string> parse_deps_prefix(std::string_view output, const fs::path& dir) {
    std::string wanted = str::to_lower(paths::to_native_utf8(dir));
    for (const auto& line : str::split_lines(output)) {
        auto at = str::to_lower(line).find(wanted);
        if (at == std::string::npos || at == 0) continue;
        std::string prefix(str::trim(std::string_view(line).substr(0, at)));
        if (!prefix.empty()) return prefix;
    }
    return std::nullopt;
}

Toolchain probe(const fs::path& cl, const fs::path& home) {
    std::optional<VisualStudio> where = vs::locate(cl, vs::installations(home));
    if (!where) {
        throw Error(std::format("{} isn't inside a Visual Studio installation (…/VC/Tools/MSVC/<toolset>/bin/…)", paths::to_utf8(cl)),
                    "install Visual Studio or its Build Tools with the C++ tools, and choose it with --toolchain msvc");
    }
    Toolchain env_only;  // the environment the probes run in
    env_only.family = Family::Msvc;
    env_only.cxx = cl;
    env_only.env = vs::capture_environment(*where);

    fs::path tmp = home / "tmp";
    fs::path probe = scratch_file(home, "msvc-probe.cpp", probe_source());
    proc::Captured r = run_cl(env_only, {"/nologo", "/EP", "/Zc:preprocessor", "/TP", paths::to_utf8(probe)}, tmp);
    if (r.exit_code != 0) {
        throw Error(std::format("probing the compiler {} failed (exit code {}):\n{}", paths::to_utf8(cl), r.exit_code,
                                tail(r.out + r.err, 20)),
                    "check in the Visual Studio Installer that the C++ tools and a Windows SDK are installed");
    }
    Toolchain tc = toolchain_from_probe(parse_probe(r.out), cl);
    tc.visual_studio = where;
    tc.env = env_only.env;

    // The prefix of /showIncludes lines, which localised installations translate (build.md §1.3).
    scratch_file(home, "msvc-deps.h", "// a header for the /showIncludes probe\n");
    fs::path deps = scratch_file(home, "msvc-deps.cpp", "#include \"msvc-deps.h\"\n");
    proc::Captured d = run_cl(tc, {"/nologo", "/c", "/Zs", "/showIncludes", paths::to_utf8(deps)}, tmp);
    tc.deps_prefix = parse_deps_prefix(d.out + d.err, tmp).value_or("Note: including file:");

    fs::path dir = cl.parent_path();
    tc.ar = dir / "lib.exe";
    tc.linker = dir / "link.exe";
    for (const fs::path& tool : {tc.ar, tc.linker}) {
        std::error_code ec;
        if (!fs::is_regular_file(tool, ec)) {
            throw Error(std::format("{} is missing next to {}", paths::to_utf8(tool.filename()), paths::to_utf8(cl)),
                        "repair the C++ tools in the Visual Studio Installer");
        }
    }
    tc.c = CCompiler{.path = cl};  // cl.exe compiles C as well (build.md §1.2)
    return tc;
}

bool accepts(const Toolchain& tc, const std::string& flag, const fs::path& home) {
    fs::path empty = scratch_file(home, "empty.cpp", "");
    proc::Captured r = run_cl(tc, {"/nologo", "/Zs", flag, paths::to_utf8(empty)}, home / "tmp");
    return r.exit_code == 0 && !(r.out + r.err).contains("D9002");
}

std::string std_flag(Toolchain& tc, const std::string& std, const fs::path& home) {
    if (auto it = tc.std_flags.find(std); it != tc.std_flags.end()) return it->second;
    static const std::map<std::string, std::vector<std::string>> spellings = {
        {"c++11", {"/std:c++14"}},  // C++14 is MSVC's oldest mode
        {"c++14", {"/std:c++14"}},
        {"c++17", {"/std:c++17"}},
        {"c++20", {"/std:c++20"}},
        {"c++23", {"/std:c++23", "/std:c++23preview", "/std:c++latest"}},
        {"c++26", {"/std:c++latest"}},
    };
    auto candidates = spellings.find(std);
    if (candidates != spellings.end()) {
        for (const auto& flag : candidates->second) {
            if (accepts(tc, flag, home)) {
                tc.std_flags[std] = flag;
                cache_toolchain(tc, home);
                return flag;
            }
        }
    }
    throw Error(std::format("{} does not support {}", tc.description(), std),
                "use a newer Visual Studio, or lower \"std\" in cpm.jsonc");
}

std::string cstd_flag(const std::string& cstd) {
    static const std::map<std::string, std::string> flags = {
        {"c89", ""}, {"gnu89", ""}, {"c99", ""}, {"gnu99", ""},  // MSVC's default C
        {"c11", "/std:c11"}, {"gnu11", "/std:c11"}, {"c17", "/std:c17"}, {"gnu17", "/std:c17"},
        {"c23", "/std:clatest"}, {"gnu23", "/std:clatest"},
    };
    auto it = flags.find(cstd);
    if (it == flags.end()) throw Error(std::format("MSVC has no flag for the C standard {}", cstd));
    return it->second;
}

}  // namespace cpm::toolchain::msvc
