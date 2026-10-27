#include "manifest/manifest.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "manifest/check.hpp"
#include "semver/range.hpp"

namespace cpm::manifest {

std::string_view to_string(TargetKind kind) {
    switch (kind) {
        case TargetKind::Executable: return "executable";
        case TargetKind::StaticLib: return "static-lib";
        case TargetKind::SharedLib: return "shared-lib";
        case TargetKind::HeaderOnly: return "header-only";
    }
    return "?";
}

std::optional<TargetKind> parse_target_kind(std::string_view text) {
    if (text == "executable") return TargetKind::Executable;
    if (text == "static-lib") return TargetKind::StaticLib;
    if (text == "shared-lib") return TargetKind::SharedLib;
    if (text == "header-only") return TargetKind::HeaderOnly;
    return std::nullopt;
}

bool valid_package_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(name.front()) || !alnum(name.back())) return false;
    if (!std::all_of(name.begin(), name.end(), [&](char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; }))
        return false;
    static constexpr std::array<std::string_view, 22> reserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::find(reserved.begin(), reserved.end(), name) == reserved.end();
}

namespace {

constexpr std::array<std::string_view, 26> kTopLevelKeys = {
    "name",    "version",       "description",  "license",    "authors",  "repository",   "homepage",
    "keywords", "private",      "std",          "cstd",       "type",     "main",         "sources",
    "include", "defines",       "publicDefines", "systemLibs", "files",   "scripts",      "dependencies",
    "devDependencies", "overrides", "profiles", "platforms",  "toolchain"};

constexpr std::array<std::string_view, 7> kPlatformKeys = {"defines", "publicDefines", "systemLibs", "sources",
                                                            "cflags",  "cxxflags",      "ldflags"};

void check_defines(Checker& c, const json::Value& obj, std::string_view key, std::vector<std::string>& out) {
    auto values = c.strings(obj, key);
    if (!values) return;
    const auto& arr = obj.find(key)->as_array();
    for (std::size_t i = 0; i < values->size(); ++i) {
        if (!valid_define((*values)[i])) {
            c.error(arr[i].span(), std::format("invalid define \"{}\"", (*values)[i]), "use NAME or NAME=VALUE");
        }
    }
    out = std::move(*values);
}

void check_system_libs(Checker& c, const json::Value& obj, std::vector<std::string>& out) {
    auto values = c.strings(obj, "systemLibs");
    if (!values) return;
    const auto& arr = obj.find("systemLibs")->as_array();
    for (std::size_t i = 0; i < values->size(); ++i) {
        if (!valid_system_lib((*values)[i])) {
            c.error(arr[i].span(), std::format("invalid system library name \"{}\"", (*values)[i]),
                    "use the bare name, e.g. \"pthread\" or \"ws2_32\" (no -l prefix or file extension)");
        }
    }
    out = std::move(*values);
}

std::string canonical_object_specifier(const json::Value& v) {
    json::Value copy = json::Value::object();
    for (const auto& m : v.as_object()) {
        if (m.key != "manifest") copy.set(m.key, m.value);
    }
    return json::write(copy, {.indent = 0, .compact = true, .sort_keys = true});
}

std::optional<DependencySpec> parse_dependency(const json::Member& m, bool dev, Checker& c) {
    DependencySpec d;
    d.name = m.key;
    d.dev = dev;
    d.span = m.value.span();
    if (!valid_package_name(m.key)) {
        c.error(m.key_span, std::format("invalid package name \"{}\"", m.key),
                "names use lowercase letters, digits, '.', '_' and '-', and start and end with a letter or digit");
        return std::nullopt;
    }

    if (m.value.is_string()) {
        const std::string& s = m.value.as_string();
        d.specifier = s;
        if (s.empty()) {
            c.error(m.value.span(), std::format("empty specifier for \"{}\"", m.key), "use a version range such as \"^1.2.0\"");
            return std::nullopt;
        }
        if (s.starts_with("file:")) {
            d.kind = DependencySpec::Kind::Path;
            d.path = s.substr(5);
            if (d.path.empty()) {
                c.error(m.value.span(), "\"file:\" needs a path", "for example \"file:../mylib\"");
                return std::nullopt;
            }
        } else if (s.starts_with("git+") || s.starts_with("github:")) {
            d.kind = DependencySpec::Kind::Git;
            std::string rest = s.starts_with("git+") ? s.substr(4) : s.substr(7);
            if (auto hash = rest.find('#'); hash != std::string::npos) {
                d.git_ref = rest.substr(hash + 1);
                rest = rest.substr(0, hash);
            }
            if (s.starts_with("github:")) {
                auto slash = rest.find('/');
                if (slash == std::string::npos || slash == 0 || slash + 1 == rest.size()) {
                    c.error(m.value.span(), "\"github:\" needs <owner>/<repo>", "for example \"github:doctest/doctest#v2.4.11\"");
                    return std::nullopt;
                }
                rest = std::format("https://github.com/{}.git", rest);
            }
            if (rest.empty()) {
                c.error(m.value.span(), "git dependency without a URL");
                return std::nullopt;
            }
            d.git_url = rest;
        } else {
            d.kind = DependencySpec::Kind::Registry;
            d.range = s;
            if (!semver::Range::parse(s)) {
                c.error(m.value.span(), std::format("invalid version range \"{}\"", s),
                        "use a range such as \"^1.2.0\", \"~1.4\" or \">=2 <3\", or file:<path>, git+<url>, github:<owner>/<repo>");
                return std::nullopt;
            }
        }
        return d;
    }

    if (!m.value.is_object()) {
        c.type_error(m.value, std::format("dependency \"{}\"", m.key), "a string or an object");
        return std::nullopt;
    }
    const json::Value& v = m.value;
    d.specifier = canonical_object_specifier(v);
    if (v.find("path")) {
        static constexpr std::array<std::string_view, 2> keys = {"path", "manifest"};
        c.unknown_keys(v, keys, std::format("path dependency \"{}\"", m.key));
        d.kind = DependencySpec::Kind::Path;
        d.path = c.string(v, "path").value_or("");
    } else if (v.find("git")) {
        static constexpr std::array<std::string_view, 6> keys = {"git", "rev", "tag", "branch", "version", "manifest"};
        c.unknown_keys(v, keys, std::format("git dependency \"{}\"", m.key));
        d.kind = DependencySpec::Kind::Git;
        d.git_url = c.string(v, "git").value_or("");
        int refs = 0;
        for (const char* k : {"rev", "tag", "branch"}) {
            if (auto r = c.string(v, k)) {
                d.git_ref = *r;
                ++refs;
            }
        }
        if (refs > 1) c.error(v.span(), "use at most one of \"rev\", \"tag\" and \"branch\"");
    } else if (v.find("version")) {
        static constexpr std::array<std::string_view, 2> keys = {"version", "registry"};
        c.unknown_keys(v, keys, std::format("registry dependency \"{}\"", m.key));
        d.kind = DependencySpec::Kind::Registry;
        d.range = c.string(v, "version").value_or("");
        d.registry = c.string(v, "registry").value_or("");
        if (!semver::Range::parse(d.range)) c.error(v.find("version")->span(), std::format("invalid version range \"{}\"", d.range));
    } else {
        c.error(v.span(), std::format("dependency \"{}\" needs \"version\", \"git\" or \"path\"", m.key));
        return std::nullopt;
    }
    if (const json::Value* overlay = v.find("manifest")) {
        if (d.kind == DependencySpec::Kind::Registry) {
            c.error(overlay->span(), "registry dependencies take their overlay from the registry index");
        } else if (!overlay->is_object()) {
            c.type_error(*overlay, "\"manifest\"", "an object");
        } else {
            d.overlay = *overlay;
        }
    }
    return d;
}

std::vector<DependencySpec> parse_dependencies(const json::Value& root, std::string_view key, bool dev, Checker& c) {
    std::vector<DependencySpec> out;
    const json::Value* deps = c.object(root, key);
    if (deps == nullptr) return out;
    for (const auto& m : deps->as_object()) {
        if (auto d = parse_dependency(m, dev, c)) out.push_back(std::move(*d));
    }
    return out;
}

}  // namespace

Manifest parse(std::string text, const fs::path& file, std::string display) {
    Manifest m;
    m.file = file;
    m.root = file.parent_path();
    m.text = std::move(text);

    json::Value root;
    try {
        root = json::parse(m.text);
    } catch (const json::ParseError& e) {
        Diagnostic d;
        d.message = e.what();
        d.loc = json::locate(m.text, {e.offset(), e.offset() + 1}, display);
        throw DiagnosticsError({d});
    }

    Checker c(m.text, display);
    if (!root.is_object()) {
        c.type_error(root, "the manifest", "a JSON object");
        throw DiagnosticsError(c.take_all());
    }
    c.unknown_keys(root, kTopLevelKeys, "cpm.jsonc");

    // name / version (required)
    if (const json::Value* v = root.find("name")) {
        if (!v->is_string()) {
            c.type_error(*v, "\"name\"", "a string");
        } else if (!valid_package_name(v->as_string())) {
            c.error(v->span(), std::format("invalid package name \"{}\"", v->as_string()),
                    "use 1-64 lowercase letters, digits, '.', '_' or '-', starting and ending with a letter or digit");
        } else {
            m.name = v->as_string();
        }
    } else {
        c.error({0, 1}, "missing required field \"name\"", "add \"name\": \"my-package\"");
    }
    if (const json::Value* v = root.find("version")) {
        if (!v->is_string()) {
            c.type_error(*v, "\"version\"", "a string");
        } else if (auto ver = semver::Version::parse(v->as_string())) {
            m.version = *ver;
        } else {
            c.error(v->span(), std::format("invalid version \"{}\"", v->as_string()),
                    "use SemVer 2.0 without a leading 'v', e.g. \"1.2.3\" or \"2.0.0-beta.1\"");
        }
    } else {
        c.error({0, 1}, "missing required field \"version\"", "add \"version\": \"0.1.0\"");
    }

    // metadata
    m.description = c.string(root, "description").value_or("");
    m.license = c.string(root, "license").value_or("");
    m.repository = c.string(root, "repository").value_or("");
    m.homepage = c.string(root, "homepage").value_or("");
    m.authors = c.strings(root, "authors").value_or(std::vector<std::string>{});
    m.keywords = c.strings(root, "keywords").value_or(std::vector<std::string>{});
    m.is_private = c.boolean(root, "private").value_or(false);

    // build settings
    if (auto s = c.string(root, "std")) {
        if (std::find(known_standards().begin(), known_standards().end(), *s) == known_standards().end()) {
            c.error(root.find("std")->span(), std::format("unknown C++ standard \"{}\"", *s),
                    "use c++11, c++14, c++17, c++20, c++23 or c++26");
        } else {
            m.std = *s;
        }
        m.std_declared = true;
    }
    if (auto s = c.string(root, "cstd")) {
        if (std::find(known_c_standards().begin(), known_c_standards().end(), *s) == known_c_standards().end()) {
            c.error(root.find("cstd")->span(), std::format("unknown C standard \"{}\"", *s),
                    "use c89, c99, c11, c17 or c23, or a GNU dialect such as gnu17");
        } else {
            m.cstd = *s;
        }
        // A C library needs no particular C++ standard (manifest.md §3.2).
        if (!m.std_declared) m.std = "c++11";
    }
    if (auto t = c.string(root, "type")) {
        if (auto kind = parse_target_kind(*t)) {
            m.type = kind;
        } else {
            c.error(root.find("type")->span(), std::format("unknown target type \"{}\"", *t),
                    "expected one of: executable, static-lib, shared-lib, header-only");
        }
    }
    // toolchain (build.md §1.1): requests only, a family and an optional version range
    if (const json::Value* tc = root.find("toolchain")) {
        std::vector<const json::Value*> items;
        if (tc->is_string()) items.push_back(tc);
        else if (tc->is_array()) for (const auto& v : tc->as_array()) items.push_back(&v);
        else c.type_error(*tc, "\"toolchain\"", "a string or an array of strings");
        if (tc->is_array() && items.empty()) c.error(tc->span(), "\"toolchain\" must name at least one compiler");
        for (const json::Value* v : items) {
            if (!v->is_string()) {
                c.type_error(*v, "a \"toolchain\" entry", "a string");
                continue;
            }
            std::string_view request = str::trim(v->as_string());
            std::size_t space = request.find_first_of(" \t");
            std::string_view family = request.substr(0, space);
            std::string_view range = space == std::string_view::npos ? "" : str::trim(request.substr(space));
            if (family != "gcc" && family != "clang" && family != "apple-clang" && family != "msvc") {
                c.error(v->span(), std::format("\"{}\" is not a compiler request", request),
                        "start with gcc, clang, apple-clang or msvc, optionally followed by a version range, such as \"gcc >=14\"");
            } else if (!range.empty() && !semver::Range::parse(range)) {
                c.error(v->span(), std::format("invalid compiler version range \"{}\"", range), "use a range such as \">=14\", \"18\" or \"^13.2\"");
            } else {
                m.toolchain.emplace_back(request);
            }
        }
    }
    if (auto main = c.string(root, "main")) {
        if (main->empty()) c.error(root.find("main")->span(), "\"main\" must not be empty");
        else m.main = *main;
    }
    if (auto src = c.strings(root, "sources")) m.sources = *src;
    if (auto inc = c.string(root, "include")) {
        if (inc->empty()) c.error(root.find("include")->span(), "\"include\" must not be empty");
        else m.include_dir = *inc;
    }
    check_defines(c, root, "defines", m.defines);
    check_defines(c, root, "publicDefines", m.public_defines);
    check_system_libs(c, root, m.system_libs);
    if (auto files = c.strings(root, "files")) m.files = *files;

    // scripts
    if (const json::Value* scripts = c.object(root, "scripts")) {
        for (const auto& s : scripts->as_object()) {
            if (!s.value.is_string()) {
                c.type_error(s.value, std::format("script \"{}\"", s.key), "a string");
                continue;
            }
            m.scripts.emplace_back(s.key, s.value.as_string());
        }
    }

    // dependencies
    m.dependencies = parse_dependencies(root, "dependencies", false, c);
    m.dev_dependencies = parse_dependencies(root, "devDependencies", true, c);
    for (const auto& d : m.dev_dependencies) {
        for (const auto& r : m.dependencies) {
            if (r.name == d.name) {
                c.error(d.span, std::format("\"{}\" is in both dependencies and devDependencies", d.name),
                        "keep it in one of them");
            }
        }
    }
    for (const auto* list : {&m.dependencies, &m.dev_dependencies}) {
        for (const auto& d : *list) {
            if (!m.name.empty() && d.name == m.name) c.error(d.span, "a package cannot depend on itself");
        }
    }

    // overrides (dependencies.md §4.6): "name" or "dependent>name", each to a version range
    if (const json::Value* overrides = c.object(root, "overrides")) {
        for (const auto& o : overrides->as_object()) {
            auto arrow = o.key.find('>');
            std::string name = arrow == std::string::npos ? o.key : o.key.substr(arrow + 1);
            std::string dependent = arrow == std::string::npos ? "" : o.key.substr(0, arrow);
            if (!valid_package_name(name) || (arrow != std::string::npos && (!valid_package_name(dependent) || dependent == name))) {
                c.error(o.key_span, std::format("invalid override \"{}\"", o.key),
                        "use a package name such as \"fmt\", or <dependent>><name> such as \"spdlog>fmt\"");
                continue;
            }
            if (!o.value.is_string()) {
                c.type_error(o.value, std::format("override \"{}\"", o.key), "a version range");
                continue;
            }
            if (!semver::Range::parse(o.value.as_string())) {
                c.error(o.value.span(), std::format("invalid version range \"{}\"", o.value.as_string()),
                        "use a range such as \"^10.2.0\", \"~1.4\" or \">=2 <3\"");
                continue;
            }
            m.overrides[o.key] = o.value.as_string();
        }
    }

    // profiles
    m.profiles = resolve_profiles(c.object(root, "profiles"), c);

    // platforms
    if (const json::Value* platforms = c.object(root, "platforms")) {
        for (const auto& p : platforms->as_object()) {
            if (p.key != "windows" && p.key != "linux" && p.key != "macos" && p.key != "unix") {
                c.error(p.key_span, std::format("unknown platform \"{}\"", p.key), "use windows, linux, macos or unix");
                continue;
            }
            if (!p.value.is_object()) {
                c.type_error(p.value, std::format("platform \"{}\"", p.key), "an object");
                continue;
            }
            c.unknown_keys(p.value, kPlatformKeys, std::format("platform \"{}\"", p.key));
            PlatformOverrides o;
            check_defines(c, p.value, "defines", o.defines);
            check_defines(c, p.value, "publicDefines", o.public_defines);
            check_system_libs(c, p.value, o.system_libs);
            o.sources = c.strings(p.value, "sources").value_or(std::vector<std::string>{});
            o.cflags = c.strings(p.value, "cflags").value_or(std::vector<std::string>{});
            o.cxxflags = c.strings(p.value, "cxxflags").value_or(std::vector<std::string>{});
            o.ldflags = c.strings(p.value, "ldflags").value_or(std::vector<std::string>{});
            m.platforms.emplace(p.key, std::move(o));
        }
    }

    if (c.has_errors()) throw DiagnosticsError(c.take_all());
    m.warnings = c.take_warnings();
    return m;
}

Manifest load(const fs::path& file, const fs::path& display_base) {
    auto text = fsx::try_read_file(file);
    if (!text) throw Error(std::format("cannot read '{}'", paths::to_utf8(file)));
    return parse(std::move(*text), fs::absolute(file).lexically_normal(), paths::display(file, display_base));
}

Manifest parse_package(const std::optional<std::string>& text, const std::optional<json::Value>& overlay,
                       const std::string& name, const std::string& fallback_version, const fs::path& file,
                       const std::string& display) {
    auto check_name = [&](const Manifest& m) {
        if (m.name != name) {
            throw Error(std::format("{} names the package \"{}\", but the dependency is called \"{}\"", display,
                                    m.name, name),
                        "use the package's own name as the dependency name");
        }
        return m;
    };
    if (text && !overlay) return check_name(parse(*text, file, display));
    if (!text && !overlay) {
        throw Error(std::format("{} has no cpm.jsonc", display),
                    "add a \"manifest\" overlay to the dependency in cpm.jsonc (manifest.md §8)");
    }

    json::Value root = json::Value::object();
    if (text) {
        try {
            root = json::parse(*text);
        } catch (const json::ParseError& e) {
            Diagnostic d;
            d.message = e.what();
            d.loc = json::locate(*text, {e.offset(), e.offset() + 1}, display);
            throw DiagnosticsError({d});
        }
        if (!root.is_object()) throw Error(std::format("{}: the manifest must be a JSON object", display));
    }
    for (const auto& m : overlay->as_object()) root.set(m.key, m.value);
    if (!root.find("name")) root.set("name", name);
    if (!root.find("version")) root.set("version", fallback_version);
    return check_name(parse(json::write(root), file, display + " (with the manifest overlay)"));
}

const std::pair<const std::string, std::string>* find_override(const std::map<std::string, std::string>& overrides,
                                                               std::string_view dependent, std::string_view name) {
    if (auto it = overrides.find(std::format("{}>{}", dependent, name)); it != overrides.end()) return &*it;
    if (auto it = overrides.find(std::string(name)); it != overrides.end()) return &*it;
    return nullptr;
}

const std::pair<const std::string, std::string>* override_for(const Manifest& project, std::string_view dependent,
                                                              const DependencySpec& requirement) {
    if (requirement.kind != DependencySpec::Kind::Registry) return nullptr;
    return find_override(project.overrides, dependent, requirement.name);
}

PlatformOverrides effective_platform(const Manifest& m, Os os) {
    PlatformOverrides out;
    auto add = [&](const char* key) {
        auto it = m.platforms.find(key);
        if (it == m.platforms.end()) return;
        const PlatformOverrides& p = it->second;
        auto append = [](std::vector<std::string>& to, const std::vector<std::string>& from) {
            to.insert(to.end(), from.begin(), from.end());
        };
        append(out.defines, p.defines);
        append(out.public_defines, p.public_defines);
        append(out.system_libs, p.system_libs);
        append(out.sources, p.sources);
        append(out.cflags, p.cflags);
        append(out.cxxflags, p.cxxflags);
        append(out.ldflags, p.ldflags);
    };
    if (is_unix(os)) add("unix");
    if (os == Os::Windows) add("windows");
    else if (os == Os::Linux) add("linux");
    else if (os == Os::MacOS) add("macos");
    return out;
}

}  // namespace cpm::manifest
