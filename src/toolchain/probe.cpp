#include "toolchain/probe.hpp"

#include <chrono>
#include <format>
#include <vector>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

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

std::optional<Family> family_from_string(std::string_view s) {
    for (Family f : {Family::Gcc, Family::Clang, Family::AppleClang, Family::Msvc, Family::ClangCl}) {
        if (to_string(f) == s) return f;
    }
    return std::nullopt;
}

std::optional<Os> os_from_string(std::string_view s) {
    for (Os o : {Os::Windows, Os::Linux, Os::MacOS, Os::FreeBSD}) {
        if (to_string(o) == s) return o;
    }
    return std::nullopt;
}

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

struct Stamp {
    std::int64_t size = 0;
    std::int64_t mtime = 0;
};

Stamp stamp_of(const fs::path& p) {
    std::error_code ec;
    Stamp s;
    s.size = static_cast<std::int64_t>(fs::file_size(p, ec));
    auto t = fs::last_write_time(p, ec);
    s.mtime = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    return s;
}

fs::path cache_file(const fs::path& home) { return home / "toolchains.json"; }

json::Value load_cache(const fs::path& home) {
    if (auto text = fsx::try_read_file(cache_file(home))) {
        try {
            json::Value v = json::parse(*text, json::kStrict);
            const json::Value* ver = v.find("toolchainsVersion");
            if (v.is_object() && ver && ver->is_int() && ver->as_int() == 1 && v.find("toolchains") &&
                v.find("toolchains")->is_object())
                return v;
        } catch (const Error&) {
        }
    }
    json::Value v = json::Value::object();
    v.set("toolchainsVersion", 1);
    v.set("toolchains", json::Value::object());
    return v;
}

json::Value to_json(const Toolchain& tc, const Stamp& stamp) {
    json::Value e = json::Value::object();
    json::Value st = json::Value::object();
    st.set("size", stamp.size);
    st.set("mtime", stamp.mtime);
    e.set("stamp", std::move(st));
    e.set("family", std::string(to_string(tc.family)));
    e.set("version", tc.version);
    e.set("target", tc.target);
    e.set("ar", paths::to_utf8(tc.ar));
    json::Value flags = json::Value::object();
    for (const auto& [k, v] : tc.std_flags) flags.set(k, v);
    e.set("stdFlags", std::move(flags));
    json::Value in = json::Value::object();
    in.set("arch", tc.arch);
    in.set("compiler", std::string(to_string(tc.family)));
    in.set("crt", tc.crt);
    in.set("eh", tc.eh);
    in.set("os", std::string(to_string(tc.os)));
    in.set("stdlib", tc.stdlib);
    in.set("stdlib.abi", tc.stdlib_abi);
    in.set("threads", tc.threads);
    e.set("inputs", std::move(in));
    return e;
}

std::optional<Toolchain> from_json(const json::Value& e, const fs::path& cxx, const Stamp& stamp) {
    try {
        const json::Value& st = e.at("stamp");
        if (st.at("size").as_int() != stamp.size || st.at("mtime").as_int() != stamp.mtime) return std::nullopt;
        const json::Value& in = e.at("inputs");
        Toolchain tc;
        auto family = family_from_string(e.at("family").as_string());
        auto os = os_from_string(in.at("os").as_string());
        if (!family || !os) return std::nullopt;
        tc.family = *family;
        tc.os = *os;
        tc.cxx = cxx;
        tc.ar = paths::from_utf8(e.at("ar").as_string());
        tc.version = e.at("version").as_string();
        tc.target = e.at("target").as_string();
        tc.arch = in.at("arch").as_string();
        tc.crt = in.at("crt").as_string();
        tc.eh = in.at("eh").as_string();
        tc.stdlib = in.at("stdlib").as_string();
        tc.stdlib_abi = in.at("stdlib.abi").as_string();
        tc.threads = in.at("threads").as_string();
        for (const auto& m : e.at("stdFlags").as_object()) tc.std_flags[m.key] = m.value.as_string();
        std::error_code ec;
        if (!fs::is_regular_file(tc.ar, ec)) return std::nullopt;
        return tc;
    } catch (const std::exception&) {
        return std::nullopt;  // malformed entry: probe again
    }
}

void store_in_cache(const fs::path& home, const Toolchain& tc) {
    json::Value cache = load_cache(home);
    cache.find("toolchains")->set(paths::to_utf8(tc.cxx), to_json(tc, stamp_of(tc.cxx)));
    try {
        fsx::write_file_atomic(cache_file(home), json::write(cache, {.sort_keys = true}) + "\n");
    } catch (const Error& e) {
        log::verbose(std::format("could not update the toolchain cache: {}", e.what()));
    }
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
    return tc.os == Os::Windows && tc.target.find("-msvc") != std::string::npos;
}

Toolchain probe_compiler(const fs::path& cxx, const fs::path& home) {
    Stamp stamp = stamp_of(cxx);
    json::Value cache = load_cache(home);
    if (const json::Value* entry = cache.find("toolchains")->find(paths::to_utf8(cxx))) {
        if (auto tc = from_json(*entry, cxx, stamp)) {
            log::trace(std::format("toolchain cache hit: {}", paths::to_utf8(cxx)));
            return *tc;
        }
    }

    log::verbose(std::format("probing {}", paths::to_utf8(cxx)));
    fs::path probe = scratch_file(home, "probe.cpp", "#include <version>\n#include <cstdlib>\n");
    proc::Command cmd{.argv = {paths::to_utf8(cxx), "-v", "-x", "c++", "-E", "-dM", paths::to_utf8(probe)}};
    proc::Captured r = proc::capture(cmd);
    if (r.exit_code != 0) {
        throw Error(std::format("probing the compiler {} failed (exit code {}):\n{}", paths::to_utf8(cxx), r.exit_code,
                                tail(r.err, 20)),
                    "check that the compiler and its standard library are installed correctly");
    }
    Toolchain tc = toolchain_from_probe(parse_probe_output(r.out, r.err), cxx);
    tc.ar = find_archiver(tc);
    store_in_cache(home, tc);
    return tc;
}

std::string std_flag(Toolchain& tc, const std::string& std, const fs::path& home) {
    if (auto it = tc.std_flags.find(std); it != tc.std_flags.end()) return it->second;

    static const std::map<std::string, std::string> provisional = {
        {"c++11", "c++0x"}, {"c++14", "c++1y"}, {"c++17", "c++1z"},
        {"c++20", "c++2a"}, {"c++23", "c++2b"}, {"c++26", "c++2c"}};
    fs::path empty = scratch_file(home, "empty.cpp", "");
    std::vector<std::string> spellings = {std};
    if (auto it = provisional.find(std); it != provisional.end()) spellings.push_back(it->second);
    for (const auto& spelling : spellings) {
        std::string flag = "-std=" + spelling;
        proc::Command cmd{.argv = {paths::to_utf8(tc.cxx), flag, "-x", "c++", "-fsyntax-only", paths::to_utf8(empty)}};
        if (proc::capture(cmd).exit_code == 0) {
            tc.std_flags[std] = flag;
            store_in_cache(home, tc);
            return flag;
        }
    }
    throw Error(std::format("{} does not support {}", tc.description(), std),
                "use a newer compiler, or lower \"std\" in cpm.jsonc");
}

}  // namespace cpm::toolchain
