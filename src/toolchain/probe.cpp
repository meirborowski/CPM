#include "toolchain/probe.hpp"

#include <format>
#include <vector>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "toolchain/msvc.hpp"
#include "toolchain/probe_cache.hpp"

namespace cpm::toolchain {

std::string_view to_string(Family family) {
    switch (family) {
        case Family::Gcc: return "gcc";
        case Family::Clang: return "clang";
        case Family::AppleClang: return "apple-clang";
        case Family::Msvc: return "msvc";
        case Family::ClangCl: return "clang-cl";
    }
    return "?";
}

std::string Toolchain::description() const { return std::format("{} {}", to_string(family), version); }

namespace {

std::string tail(std::string_view text, std::size_t lines) {
    auto all = str::split_lines(text);
    std::size_t start = all.size() > lines ? all.size() - lines : 0;
    std::vector<std::string> kept(all.begin() + static_cast<std::ptrdiff_t>(start), all.end());
    return str::join(kept, "\n");
}

fs::path find_archiver(const Toolchain& tc) {
    fs::path dir = tc.cxx.parent_path();
    std::string stem = paths::to_utf8(tc.cxx.stem());
    std::string ext = paths::to_utf8(tc.cxx.extension());
    std::vector<std::string> candidates;
    auto replace_last = [&](std::string_view from, std::string_view to) {
        auto pos = stem.rfind(from);
        if (pos != std::string::npos) candidates.push_back(stem.substr(0, pos) + std::string(to) + stem.substr(pos + from.size()));
    };
    if (tc.family == Family::Gcc) {
        replace_last("g++", "gcc-ar");
        replace_last("c++", "gcc-ar");
        candidates.push_back("gcc-ar");
    } else {
        replace_last("clang++", "llvm-ar");
        candidates.push_back("llvm-ar");
    }
    candidates.push_back("ar");
    for (const auto& name : candidates) {
        fs::path p = dir / paths::from_utf8(name + ext);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    for (const auto& name : candidates) {
        if (auto p = proc::find_program(name)) return *p;
    }
    throw Error(std::format("no archiver found for {}", paths::to_utf8(tc.cxx)),
            "install binutils (ar) or LLVM (llvm-ar) next to the compiler");
}

fs::path scratch_file(const fs::path& home, const char* name, std::string_view content) {
    fs::path p = home / "tmp" / name;
    fsx::write_if_changed(p, content);
    return p;
}

}  // namespace

ProbeOutput parse_probe_output(std::string_view macros_text, std::string_view verbose_text) {
    ProbeOutput out;
    for (const auto& line : str::split_lines(macros_text)) {
        std::string_view l = line;
        if (!l.starts_with("#define ")) continue;
        l.remove_prefix(8);
        auto sp = l.find(' ');
        std::string name(l.substr(0, sp));
        std::string value = sp == std::string_view::npos ? std::string() : std::string(str::trim(l.substr(sp + 1)));
        out.macros[name] = value;
    }
    for (const auto& line : str::split_lines(verbose_text)) {
        std::string_view l = str::trim(line);
        if (l.starts_with("Target: ")) out.target = std::string(str::trim(l.substr(8)));
        else if (l.starts_with("Thread model: ")) out.thread_model = std::string(str::trim(l.substr(14)));
        else if (l.starts_with("Apple clang") || l.starts_with("Apple LLVM")) out.apple = true;
    }
    return out;
}

Toolchain toolchain_from_probe(const ProbeOutput& p, const fs::path& cxx) {
    auto has = [&](const char* m) { return p.macros.contains(m); };
    auto get = [&](const char* m) {
        auto it = p.macros.find(m);
        return it == p.macros.end() ? std::string() : it->second;
    };
    Toolchain tc;
    tc.cxx = cxx;
    if (has("__clang__")) {
        tc.family = (p.apple || has("__apple_build_version__")) ? Family::AppleClang : Family::Clang;
        tc.version = std::format("{}.{}.{}", get("__clang_major__"), get("__clang_minor__"), get("__clang_patchlevel__"));
    } else if (has("__GNUC__")) {
        tc.family = Family::Gcc;
        tc.version = std::format("{}.{}.{}", get("__GNUC__"), get("__GNUC_MINOR__"), get("__GNUC_PATCHLEVEL__"));
    } else {
        throw Error(std::format("'{}' does not look like GCC or Clang", paths::to_utf8(cxx)),
                    "CPM currently supports g++ and clang++");
    }

    if (has("_WIN32")) tc.os = Os::Windows;
    else if (has("__APPLE__")) tc.os = Os::MacOS;
    else if (has("__FreeBSD__")) tc.os = Os::FreeBSD;
    else tc.os = Os::Linux;

    if (has("__x86_64__") || has("_M_X64")) tc.arch = "x86_64";
    else if (has("__aarch64__") || has("_M_ARM64")) tc.arch = "aarch64";
    else if (has("__i386__") || has("_M_IX86")) tc.arch = "x86";
    else if (has("__arm__")) tc.arch = "arm";
    else tc.arch = "unknown";

    tc.target = p.target.empty() ? std::format("{}-unknown-{}", tc.arch, to_string(tc.os)) : p.target;

    if (has("_LIBCPP_VERSION")) {
        tc.stdlib = "libc++";
        tc.stdlib_abi = "v" + (has("_LIBCPP_ABI_VERSION") ? get("_LIBCPP_ABI_VERSION") : std::string("1"));
    } else if (has("__GLIBCXX__")) {
        tc.stdlib = "libstdc++";
        tc.stdlib_abi = get("_GLIBCXX_USE_CXX11_ABI") == "0" ? "cxx98" : "cxx11";
    } else {
        tc.stdlib = "unknown";
        tc.stdlib_abi = "unknown";
    }

    switch (tc.os) {
        case Os::Windows: tc.crt = has("_UCRT") ? "ucrt" : "msvcrt"; break;
        case Os::MacOS: tc.crt = "libsystem"; break;
        case Os::FreeBSD: tc.crt = "libc"; break;
        case Os::Linux: tc.crt = has("__GLIBC__") ? "glibc" : "musl"; break;
    }

    if (has("__SEH__")) tc.eh = "seh";
    else if (has("__USING_SJLJ_EXCEPTIONS__")) tc.eh = "sjlj";
    else if (tc.os == Os::Windows) tc.eh = "dwarf";
    else tc.eh = "itanium";

    tc.threads = p.thread_model.empty() ? "unknown" : p.thread_model;
    return tc;
}

bool targets_msvc(const Toolchain& tc) {
    return tc.family != Family::Msvc && tc.os == Os::Windows && tc.target.find("-msvc") != std::string::npos;
}

ProbeOutput run_probe(const fs::path& compiler, Language language, const fs::path& home) {
    bool c = language == Language::C;
    fs::path probe = c ? scratch_file(home, "probe.c", "#include <stdlib.h>\n")
                       : scratch_file(home, "probe.cpp", "#include <version>\n#include <cstdlib>\n");
    proc::Command cmd{.argv = {paths::to_utf8(compiler), "-v", "-x", c ? "c" : "c++", "-E", "-dM", paths::to_utf8(probe)}};
    proc::Captured r = proc::capture(cmd);
    if (r.exit_code != 0) {
        throw Error(std::format("probing the compiler {} failed (exit code {}):\n{}", paths::to_utf8(compiler),
                                r.exit_code, tail(r.err, 20)),
                    std::format("check that the compiler and its {} library are installed correctly",
                                c ? "C" : "standard"));
    }
    return parse_probe_output(r.out, r.err);
}

bool accepts_flag(const fs::path& compiler, Language language, const std::string& flag, const fs::path& home) {
    bool c = language == Language::C;
    fs::path empty = c ? scratch_file(home, "empty.c", "") : scratch_file(home, "empty.cpp", "");
    proc::Command cmd{.argv = {paths::to_utf8(compiler), flag, "-x", c ? "c" : "c++", "-fsyntax-only", paths::to_utf8(empty)}};
    return proc::capture(cmd).exit_code == 0;
}

Toolchain probe_compiler(const fs::path& cxx, const fs::path& home) {
    if (auto tc = cached_toolchain(cxx, home)) {
        log::trace(std::format("toolchain cache hit: {}", paths::to_utf8(cxx)));
        return *tc;
    }

    log::verbose(std::format("probing {}", paths::to_utf8(cxx)));
    Toolchain tc;
    if (msvc::is_cl(cxx)) {
        tc = msvc::probe(cxx, home);
    } else {
        tc = toolchain_from_probe(run_probe(cxx, Language::Cxx, home), cxx);
        tc.ar = find_archiver(tc);
    }
    cache_toolchain(tc, home);
    return tc;
}

std::string std_flag(Toolchain& tc, const std::string& std, const fs::path& home) {
    if (tc.family == Family::Msvc) return msvc::std_flag(tc, std, home);
    if (auto it = tc.std_flags.find(std); it != tc.std_flags.end()) return it->second;

    static const std::map<std::string, std::string> provisional = {
        {"c++11", "c++0x"}, {"c++14", "c++1y"}, {"c++17", "c++1z"},
        {"c++20", "c++2a"}, {"c++23", "c++2b"}, {"c++26", "c++2c"}};
    std::vector<std::string> spellings = {std};
    if (auto it = provisional.find(std); it != provisional.end()) spellings.push_back(it->second);
    for (const auto& spelling : spellings) {
        std::string flag = "-std=" + spelling;
        if (accepts_flag(tc.cxx, Language::Cxx, flag, home)) {
            tc.std_flags[std] = flag;
            cache_toolchain(tc, home);
            return flag;
        }
    }
    throw Error(std::format("{} does not support {}", tc.description(), std),
                "use a newer compiler, or lower \"std\" in cpm.jsonc");
}

}  // namespace cpm::toolchain
