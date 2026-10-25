#include "toolchain/probe_cache.hpp"

#include <chrono>
#include <cstdint>
#include <format>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

namespace cpm::toolchain {

namespace fs = std::filesystem;

namespace {

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

json::Value stamp_json(const fs::path& p) {
    Stamp stamp = stamp_of(p);
    json::Value st = json::Value::object();
    st.set("size", stamp.size);
    st.set("mtime", stamp.mtime);
    return st;
}

bool stamp_matches(const json::Value& st, const fs::path& p) {
    Stamp stamp = stamp_of(p);
    return st.at("size").as_int() == stamp.size && st.at("mtime").as_int() == stamp.mtime;
}

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

json::Value flags_json(const std::map<std::string, std::string>& flags) {
    json::Value out = json::Value::object();
    for (const auto& [k, v] : flags) out.set(k, v);
    return out;
}

std::map<std::string, std::string> flags_from_json(const json::Value& v) {
    std::map<std::string, std::string> out;
    for (const auto& m : v.as_object()) out[m.key] = m.value.as_string();
    return out;
}

json::Value to_json(const Toolchain& tc) {
    json::Value e = json::Value::object();
    e.set("stamp", stamp_json(tc.cxx));
    e.set("family", std::string(to_string(tc.family)));
    e.set("version", tc.version);
    e.set("target", tc.target);
    e.set("ar", paths::to_utf8(tc.ar));
    e.set("stdFlags", flags_json(tc.std_flags));
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
    if (tc.c) {
        json::Value c = json::Value::object();
        c.set("path", paths::to_utf8(tc.c->path));
        c.set("stamp", stamp_json(tc.c->path));
        c.set("cstdFlags", flags_json(tc.c->cstd_flags));
        e.set("c", std::move(c));
    }
    return e;
}

// The C compiler recorded in an entry, unless its file changed or is gone.
std::optional<CCompiler> c_compiler_from_json(const json::Value& c) {
    CCompiler cc;
    cc.path = paths::from_utf8(c.at("path").as_string());
    std::error_code ec;
    if (!fs::is_regular_file(cc.path, ec) || !stamp_matches(c.at("stamp"), cc.path)) return std::nullopt;
    cc.cstd_flags = flags_from_json(c.at("cstdFlags"));
    return cc;
}

std::optional<Toolchain> from_json(const json::Value& e, const fs::path& cxx) {
    try {
        if (!stamp_matches(e.at("stamp"), cxx)) return std::nullopt;
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
        tc.std_flags = flags_from_json(e.at("stdFlags"));
        if (const json::Value* c = e.find("c")) tc.c = c_compiler_from_json(*c);
        std::error_code ec;
        if (!fs::is_regular_file(tc.ar, ec)) return std::nullopt;
        return tc;
    } catch (const std::exception&) {
        return std::nullopt;  // malformed entry: probe again
    }
}

}  // namespace

std::optional<Toolchain> cached_toolchain(const fs::path& cxx, const fs::path& home) {
    json::Value cache = load_cache(home);
    const json::Value* entry = cache.find("toolchains")->find(paths::to_utf8(cxx));
    if (entry == nullptr) return std::nullopt;
    return from_json(*entry, cxx);
}

void cache_toolchain(const Toolchain& tc, const fs::path& home) {
    json::Value cache = load_cache(home);
    cache.find("toolchains")->set(paths::to_utf8(tc.cxx), to_json(tc));
    try {
        fsx::write_file_atomic(cache_file(home), json::write(cache, {.sort_keys = true}) + "\n");
    } catch (const Error& e) {
        log::verbose(std::format("could not update the toolchain cache: {}", e.what()));
    }
}

}  // namespace cpm::toolchain
