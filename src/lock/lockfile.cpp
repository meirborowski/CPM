#include "lock/lockfile.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "manifest/check.hpp"
#include "semver/range.hpp"

namespace cpm::lock {

std::string_view kind_of(const Resolution& r) {
    if (std::holds_alternative<RegistryResolution>(r)) return "registry";
    if (std::holds_alternative<GitResolution>(r)) return "git";
    return "path";
}

std::string Package::key() const { return name + "@" + version.to_string(); }

const Package* Lockfile::find(std::string_view name) const {
    for (const auto& [key, pkg] : packages) {
        if (pkg.name == name) return &pkg;
    }
    return nullptr;
}

namespace {

const char* kCorruptHint = "delete cpm-lock.json and run 'cpm install' to recreate it";

bool is_hex(std::string_view s, std::size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<semver::Version> version_at(manifest::Checker& c, const json::Value& v, std::string_view what) {
    if (!v.is_string()) {
        c.type_error(v, what, "a version string");
        return std::nullopt;
    }
    auto ver = semver::Version::parse(v.as_string());
    if (!ver) c.error(v.span(), std::format("{} is not a valid version: \"{}\"", what, v.as_string()), kCorruptHint);
    return ver;
}

std::optional<Resolution> read_resolution(manifest::Checker& c, const json::Value& v) {
    if (!v.is_object()) {
        c.type_error(v, "\"resolution\"", "an object");
        return std::nullopt;
    }
    auto type = c.string(v, "type");
    auto get = [&](const char* key) -> std::string {
        auto s = c.string(v, key);
        if (!s) c.error(v.span(), std::format("resolution is missing \"{}\"", key), kCorruptHint);
        return s.value_or("");
    };
    if (type == "registry") {
        RegistryResolution r{get("registry"), get("url"), get("integrity")};
        if (!r.integrity.starts_with("sha256:") || !is_hex(std::string_view(r.integrity).substr(7), 64))
            c.error(v.span(), "\"integrity\" must be \"sha256:\" followed by 64 lowercase hex digits", kCorruptHint);
        return r;
    }
    if (type == "git") {
        GitResolution r{get("url"), c.string(v, "ref").value_or(""), get("commit")};
        if (!is_hex(r.commit, 40)) c.error(v.span(), "\"commit\" must be 40 lowercase hex digits", kCorruptHint);
        return r;
    }
    if (type == "path") return PathResolution{get("path")};
    c.error(v.span(), "resolution \"type\" must be registry, git or path", kCorruptHint);
    return std::nullopt;
}

void read_root_deps(manifest::Checker& c, const json::Value& importer, const char* key,
                    std::map<std::string, RootDependency>& out) {
    const json::Value* deps = importer.find(key);
    if (deps == nullptr) {
        c.error(importer.span(), std::format("importer is missing \"{}\"", key), kCorruptHint);
        return;
    }
    if (!deps->is_object()) {
        c.type_error(*deps, std::format("\"{}\"", key), "an object");
        return;
    }
    for (const auto& m : deps->as_object()) {
        auto spec = c.string(m.value, "specifier");
        const json::Value* ver = m.value.find("version");
        if (!spec || ver == nullptr) {
            c.error(m.value.span(), std::format("\"{}\" needs \"specifier\" and \"version\"", m.key), kCorruptHint);
            continue;
        }
        if (auto v = version_at(c, *ver, std::format("version of \"{}\"", m.key))) out[m.key] = {*spec, *v};
    }
}

// An optional object of strings ("overrides", "overridden").
void read_ranges(manifest::Checker& c, const json::Value& parent, const char* key, std::map<std::string, std::string>& out) {
    const json::Value* map = c.object(parent, key);
    if (map == nullptr) return;
    for (const auto& m : map->as_object()) {
        if (m.value.is_string()) out[m.key] = m.value.as_string();
        else c.type_error(m.value, std::format("\"{}\" in \"{}\"", m.key, key), "a string");
    }
}

json::Value ranges_json(const std::map<std::string, std::string>& ranges) {
    json::Value v = json::Value::object();
    for (const auto& [key, range] : ranges) v.set(key, range);
    return v;
}

json::Value resolution_json(const Resolution& r) {
    json::Value v = json::Value::object();  // keys in sorted order
    if (const auto* reg = std::get_if<RegistryResolution>(&r)) {
        v.set("integrity", reg->integrity);
        v.set("registry", reg->registry);
        v.set("type", "registry");
        v.set("url", reg->url);
    } else if (const auto* git = std::get_if<GitResolution>(&r)) {
        v.set("commit", git->commit);
        if (!git->ref.empty()) v.set("ref", git->ref);
        v.set("type", "git");
        v.set("url", git->url);
    } else {
        v.set("path", std::get<PathResolution>(r).path);
        v.set("type", "path");
    }
    return v;
}

json::Value root_deps_json(const std::map<std::string, RootDependency>& deps) {
    json::Value v = json::Value::object();
    for (const auto& [name, d] : deps) {
        json::Value e = json::Value::object();
        e.set("specifier", d.specifier);
        e.set("version", d.version.to_string());
        v.set(name, std::move(e));
    }
    return v;
}

}  // namespace

Lockfile parse(std::string_view text, const std::string& display) {
    json::Value root;
    try {
        root = json::parse(text, json::kStrict);
    } catch (const json::ParseError& e) {
        Diagnostic d;
        d.message = std::format("cpm-lock.json is not valid JSON: {}", e.what());
        d.hint = kCorruptHint;
        d.loc = json::locate(text, {e.offset(), e.offset() + 1}, display);
        throw DiagnosticsError({d});
    }
    manifest::Checker c(text, display);
    if (!root.is_object()) {
        c.type_error(root, "cpm-lock.json", "a JSON object");
        throw DiagnosticsError(c.take_all());
    }
    const json::Value* ver = root.find("lockfileVersion");
    if (ver == nullptr || !ver->is_int()) {
        c.error(root.span(), "missing \"lockfileVersion\"", kCorruptHint);
        throw DiagnosticsError(c.take_all());
    }
    if (ver->as_int() > kLockfileVersion) {
        throw Error(std::format("cpm-lock.json has lockfileVersion {}, but this CPM understands {}", ver->as_int(),
                                kLockfileVersion),
                    "upgrade CPM");
    }
    if (ver->as_int() != kLockfileVersion) c.error(ver->span(), "unsupported \"lockfileVersion\"", kCorruptHint);

    Lockfile lock;
    const json::Value* importers = c.object(root, "importers");
    const json::Value* dot = importers ? importers->find(".") : nullptr;
    if (dot == nullptr || !dot->is_object()) {
        c.error(root.span(), "missing importers[\".\"]", kCorruptHint);
    } else {
        read_root_deps(c, *dot, "dependencies", lock.dependencies);
        read_root_deps(c, *dot, "devDependencies", lock.dev_dependencies);
        read_ranges(c, *dot, "overrides", lock.overrides);
    }

    if (const json::Value* packages = c.object(root, "packages")) {
        for (const auto& m : packages->as_object()) {
            const json::Value& e = m.value;
            if (!e.is_object()) {
                c.type_error(e, std::format("package \"{}\"", m.key), "an object");
                continue;
            }
            Package p;
            auto at = m.key.rfind('@');
            auto key_version = at == std::string::npos ? std::nullopt : semver::Version::parse(m.key.substr(at + 1));
            if (at == std::string::npos || at == 0 || !key_version || !manifest::valid_package_name(m.key.substr(0, at))) {
                c.error(m.key_span, std::format("invalid package key \"{}\"", m.key), "keys are \"name@version\"");
                continue;
            }
            p.name = m.key.substr(0, at);
            p.version = *key_version;
            if (const json::Value* r = e.find("resolution")) {
                if (auto res = read_resolution(c, *r)) p.resolution = *res;
            } else {
                c.error(e.span(), std::format("package \"{}\" has no \"resolution\"", m.key), kCorruptHint);
            }
            if (auto type = c.string(e, "type")) {
                auto kind = manifest::parse_target_kind(*type);
                if (!kind || *kind == manifest::TargetKind::Executable)
                    c.error(e.find("type")->span(), std::format("invalid package type \"{}\"", *type), kCorruptHint);
                else p.type = *kind;
            }
            p.std = c.string(e, "std").value_or("c++20");
            p.dev = c.boolean(e, "dev").value_or(false);
            if (const json::Value* deps = c.object(e, "dependencies")) {
                for (const auto& d : deps->as_object()) {
                    if (auto v = version_at(c, d.value, std::format("dependency \"{}\"", d.key))) p.dependencies[d.key] = *v;
                }
            }
            read_ranges(c, e, "overridden", p.overridden);
            lock.packages.emplace(m.key, std::move(p));
        }
    }
    if (c.has_errors()) throw DiagnosticsError(c.take_all());
    return lock;
}

std::string serialize(const Lockfile& lock) {
    json::Value importer = json::Value::object();
    importer.set("dependencies", root_deps_json(lock.dependencies));
    importer.set("devDependencies", root_deps_json(lock.dev_dependencies));
    if (!lock.overrides.empty()) importer.set("overrides", ranges_json(lock.overrides));
    json::Value importers = json::Value::object();
    importers.set(".", std::move(importer));

    json::Value packages = json::Value::object();  // std::map: keys already sorted
    for (const auto& [key, p] : lock.packages) {
        json::Value e = json::Value::object();
        if (!p.dependencies.empty()) {
            json::Value deps = json::Value::object();
            for (const auto& [name, v] : p.dependencies) deps.set(name, v.to_string());
            e.set("dependencies", std::move(deps));
        }
        if (p.dev) e.set("dev", true);
        if (!p.overridden.empty()) e.set("overridden", ranges_json(p.overridden));
        e.set("resolution", resolution_json(p.resolution));
        e.set("std", p.std);
        e.set("type", std::string(manifest::to_string(p.type)));
        packages.set(key, std::move(e));
    }

    json::Value top = json::Value::object();  // fixed top-level order
    top.set("lockfileVersion", kLockfileVersion);
    top.set("importers", std::move(importers));
    top.set("packages", std::move(packages));
    return json::write(top) + "\n";
}

std::optional<Lockfile> load(const fs::path& root, const fs::path& display_base) {
    fs::path file = root / "cpm-lock.json";
    auto text = fsx::try_read_file(file);
    if (!text) return std::nullopt;
    return parse(*text, paths::display(file, display_base));
}

bool save(const fs::path& root, const Lockfile& lock) { return fsx::write_if_changed(root / "cpm-lock.json", serialize(lock)); }

std::optional<std::string> staleness(const Lockfile& lock, const manifest::Manifest& root,
                                     const std::function<manifest::Manifest(const Package&)>& read_path_package) {
    // 2. The importer matches the manifest's names and specifiers exactly.
    auto compare_importer = [&](const std::vector<manifest::DependencySpec>& specs,
                                const std::map<std::string, RootDependency>& locked,
                                const char* field) -> std::optional<std::string> {
        for (const auto& s : specs) {
            auto it = locked.find(s.name);
            if (it == locked.end()) return std::format("{} \"{}\" is not in the lockfile", field, s.name);
            if (it->second.specifier != s.specifier)
                return std::format("the specifier of \"{}\" changed ({} -> {})", s.name, it->second.specifier, s.specifier);
        }
        for (const auto& [name, d] : locked) {
            if (std::none_of(specs.begin(), specs.end(), [&](const manifest::DependencySpec& s) { return s.name == name; }))
                return std::format("\"{}\" was removed from {}", name, field);
        }
        return std::nullopt;
    };
    if (auto why = compare_importer(root.dependencies, lock.dependencies, "dependencies")) return why;
    if (auto why = compare_importer(root.dev_dependencies, lock.dev_dependencies, "devDependencies")) return why;
    if (root.overrides != lock.overrides) return std::string("the overrides changed");

    // 3. Every reference resolves and every package is reachable; otherwise the file is corrupt.
    std::set<std::string> reachable;
    std::vector<std::string> todo;
    for (const auto* deps : {&lock.dependencies, &lock.dev_dependencies}) {
        for (const auto& [name, d] : *deps) todo.push_back(name + "@" + d.version.to_string());
    }
    while (!todo.empty()) {
        std::string key = todo.back();
        todo.pop_back();
        auto it = lock.packages.find(key);
        if (it == lock.packages.end()) throw Error(std::format("cpm-lock.json refers to {}, which it does not list", key), kCorruptHint);
        if (!reachable.insert(key).second) continue;
        for (const auto& [name, v] : it->second.dependencies) todo.push_back(name + "@" + v.to_string());
    }
    for (const auto& [key, p] : lock.packages) {
        if (!reachable.contains(key)) throw Error(std::format("cpm-lock.json lists {}, which nothing depends on", key), kCorruptHint);
    }

    // 4. Path packages can change on disk: re-check them against their current manifests.
    for (const auto& [key, p] : lock.packages) {
        if (!std::holds_alternative<PathResolution>(p.resolution)) continue;
        manifest::Manifest m = read_path_package(p);
        if (m.version != p.version) return std::format("{} changed its version to {}", key, m.version.to_string());
        if (m.std != p.std) return std::format("{} changed its \"std\" to {}", key, m.std);
        if (m.type && *m.type != p.type) return std::format("{} changed its \"type\"", key);
        std::set<std::string> now, then;
        for (const auto& d : m.dependencies) now.insert(d.name);
        for (const auto& [name, v] : p.dependencies) then.insert(name);
        if (now != then) return std::format("the dependencies of {} changed", key);
        for (const auto& d : m.dependencies) {
            const Package* locked = lock.find(d.name);
            if (locked == nullptr) return std::format("{} needs {}, which is not locked", key, d.name);
            std::string_view want = d.kind == manifest::DependencySpec::Kind::Registry ? "registry"
                                    : d.kind == manifest::DependencySpec::Kind::Git ? "git" : "path";
            if (kind_of(locked->resolution) != want)
                return std::format("{} now wants {} from a {} source", key, d.name, want);
            if (d.kind == manifest::DependencySpec::Kind::Registry) {
                const auto* o = manifest::override_for(root, p.name, d);
                const std::string& wanted = o ? o->second : d.range;
                auto range = semver::Range::parse(wanted);
                if (!range || !range->contains(locked->version))
                    return std::format("{} needs {} {}, but {} is locked", key, d.name, wanted, locked->version.to_string());
            }
        }
    }
    return std::nullopt;
}

}  // namespace cpm::lock
