#include "manifest/profile.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <set>

#include "manifest/check.hpp"

namespace cpm::manifest {

bool valid_define(std::string_view d) {
    std::string_view name = d.substr(0, d.find('='));
    if (name.empty()) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(name[0])) return false;
    return std::all_of(name.begin(), name.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

bool valid_system_lib(std::string_view name) {
    if (name.empty() || name.front() == '-') return false;  // a flag such as -lm, not a name
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '+' || c == '-';
    });
}

std::map<std::string, Profile> builtin_profiles() {
    Profile debug;
    debug.name = "debug";
    Profile release;
    release.name = "release";
    release.opt = "3";
    release.debug = false;
    release.assertions = false;
    return {{"debug", debug}, {"release", release}};
}

namespace {

constexpr std::array<std::string_view, 14> kProfileKeys = {
    "inherits", "opt", "debug", "assertions", "lto", "sanitizers", "runtime",
    "debugRuntime", "warnings", "werror", "defines", "cflags", "cxxflags", "ldflags"};

constexpr std::array<std::string_view, 5> kSanitizers = {"address", "undefined", "thread", "memory", "leak"};

struct Patch {
    std::optional<std::string> inherits, opt, runtime, warnings;
    std::optional<bool> debug, assertions, lto, werror, debug_runtime;
    std::optional<std::vector<std::string>> sanitizers, defines, cflags, cxxflags, ldflags;
    json::Span span;
};

bool valid_profile_name(std::string_view n) {
    if (n.empty() || n.size() > 32 || !(n[0] >= 'a' && n[0] <= 'z')) return false;
    return std::all_of(n.begin(), n.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

Patch read_patch(const std::string& name, const json::Value& v, Checker& c) {
    Patch p;
    p.span = v.span();
    c.unknown_keys(v, kProfileKeys, std::format("profile \"{}\"", name));
    p.inherits = c.string(v, "inherits");
    if (const json::Value* opt = v.find("opt")) {
        if (opt->is_int() && opt->as_int() >= 0 && opt->as_int() <= 3) {
            p.opt = std::to_string(opt->as_int());
        } else if (opt->is_string() && (opt->as_string() == "0" || opt->as_string() == "1" ||
                                        opt->as_string() == "2" || opt->as_string() == "3" ||
                                        opt->as_string() == "s" || opt->as_string() == "z")) {
            p.opt = opt->as_string();
        } else {
            c.error(opt->span(), "\"opt\" must be 0, 1, 2, 3, \"s\" or \"z\"");
        }
    }
    p.debug = c.boolean(v, "debug");
    p.assertions = c.boolean(v, "assertions");
    p.lto = c.boolean(v, "lto");
    p.werror = c.boolean(v, "werror");
    p.debug_runtime = c.boolean(v, "debugRuntime");
    if (auto r = c.string(v, "runtime")) {
        if (*r == "dynamic" || *r == "static") p.runtime = r;
        else c.error(v.find("runtime")->span(), std::format("unknown runtime \"{}\"", *r), "use \"dynamic\" or \"static\"");
    }
    if (auto w = c.string(v, "warnings")) {
        if (*w == "none" || *w == "default" || *w == "strict") p.warnings = w;
        else c.error(v.find("warnings")->span(), std::format("unknown warning level \"{}\"", *w),
                     "use \"none\", \"default\" or \"strict\"");
    }
    if (auto s = c.strings(v, "sanitizers")) {
        const json::Value& arr = *v.find("sanitizers");
        for (std::size_t i = 0; i < s->size(); ++i) {
            if (std::find(kSanitizers.begin(), kSanitizers.end(), (*s)[i]) == kSanitizers.end()) {
                c.error(arr.as_array()[i].span(), std::format("unknown sanitizer \"{}\"", (*s)[i]),
                        "use address, undefined, thread, memory or leak");
            }
        }
        p.sanitizers = s;
    }
    if (auto d = c.strings(v, "defines")) {
        const json::Value& arr = *v.find("defines");
        for (std::size_t i = 0; i < d->size(); ++i) {
            if (!valid_define((*d)[i])) c.error(arr.as_array()[i].span(), std::format("invalid define \"{}\"", (*d)[i]),
                                                "use NAME or NAME=VALUE");
        }
        p.defines = d;
    }
    p.cflags = c.strings(v, "cflags");
    p.cxxflags = c.strings(v, "cxxflags");
    p.ldflags = c.strings(v, "ldflags");
    return p;
}

void append_unique(std::vector<std::string>& to, const std::vector<std::string>& from) {
    for (const auto& item : from) {
        if (std::find(to.begin(), to.end(), item) == to.end()) to.push_back(item);
    }
}

Profile apply(Profile base, const Patch& p, const std::string& name) {
    base.name = name;
    if (p.opt) base.opt = *p.opt;
    if (p.runtime) base.runtime = *p.runtime;
    if (p.warnings) base.warnings = *p.warnings;
    if (p.debug) base.debug = *p.debug;
    if (p.assertions) base.assertions = *p.assertions;
    if (p.lto) base.lto = *p.lto;
    if (p.werror) base.werror = *p.werror;
    if (p.debug_runtime) base.debug_runtime = *p.debug_runtime;
    if (p.sanitizers) append_unique(base.sanitizers, *p.sanitizers);
    if (p.defines) append_unique(base.defines, *p.defines);
    if (p.cflags) append_unique(base.cflags, *p.cflags);
    if (p.cxxflags) append_unique(base.cxxflags, *p.cxxflags);
    if (p.ldflags) append_unique(base.ldflags, *p.ldflags);
    return base;
}

}  // namespace

std::map<std::string, Profile> resolve_profiles(const json::Value* profiles, Checker& c) {
    std::map<std::string, Profile> result = builtin_profiles();
    if (profiles == nullptr) return result;

    std::map<std::string, Patch> patches;
    for (const auto& m : profiles->as_object()) {
        if (!m.value.is_object()) {
            c.type_error(m.value, std::format("profile \"{}\"", m.key), "an object");
            continue;
        }
        bool builtin = m.key == "debug" || m.key == "release";
        if (!builtin && !valid_profile_name(m.key)) {
            c.error(m.key_span, std::format("invalid profile name \"{}\"", m.key),
                    "use 1-32 lowercase letters, digits or '-', starting with a letter");
            continue;
        }
        Patch p = read_patch(m.key, m.value, c);
        if (!builtin && !p.inherits) {
            c.error(m.key_span, std::format("custom profile \"{}\" needs \"inherits\"", m.key),
                    "for example \"inherits\": \"release\"");
            continue;
        }
        if (builtin && p.inherits) {
            c.error(m.value.find("inherits")->span(),
                    std::format("built-in profile \"{}\" cannot inherit from another profile", m.key));
            continue;
        }
        patches.emplace(m.key, std::move(p));
    }

    // Built-ins first: they may be patched, and custom profiles inherit the patched versions.
    for (const char* name : {"debug", "release"}) {
        if (auto it = patches.find(name); it != patches.end()) result[name] = apply(result[name], it->second, name);
    }

    std::set<std::string> in_progress, failed;
    std::function<const Profile*(const std::string&)> resolve = [&](const std::string& name) -> const Profile* {
        if (auto it = result.find(name); it != result.end()) return &it->second;
        auto pit = patches.find(name);
        if (pit == patches.end() || failed.contains(name)) return nullptr;
        if (!in_progress.insert(name).second) {
            c.error(pit->second.span, std::format("profile inheritance cycle through \"{}\"", name));
            failed.insert(name);
            return nullptr;
        }
        const std::string& parent_name = *pit->second.inherits;
        const Profile* parent = resolve(parent_name);
        in_progress.erase(name);
        if (parent == nullptr) {
            if (!patches.contains(parent_name) && !result.contains(parent_name))
                c.error(pit->second.span,
                        std::format("profile \"{}\" inherits from unknown profile \"{}\"", name, parent_name));
            failed.insert(name);
            return nullptr;
        }
        Profile resolved = apply(*parent, pit->second, name);
        return &result.emplace(name, std::move(resolved)).first->second;
    };
    for (const auto& [name, patch] : patches) resolve(name);
    return result;
}

}  // namespace cpm::manifest
