#include "install/registry.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "install/fetch.hpp"
#include "install/store.hpp"
#include "json/parse.hpp"
#include "semver/range.hpp"

namespace cpm::install {

const RegistryVersion* RegistryPackage::find(const semver::Version& v) const {
    for (const auto& rv : versions) {
        if (rv.version == v) return &rv;
    }
    return nullptr;
}

namespace {

constexpr auto kIndexTtl = std::chrono::minutes(5);

[[noreturn]] void invalid(const std::string& name, const std::string& why) {
    throw Error(std::format("the registry index for \"{}\" is invalid: {}", name, why),
                "the registry's index/<name>.json does not follow docs/spec/registry.md §3");
}

// Never null: a missing member makes the index invalid.
const json::Value* member(const json::Value& obj, const char* key, const std::string& name, const std::string& where) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) invalid(name, std::format("{} has no \"{}\"", where, key));
    return v;
}

std::string string_member(const json::Value& obj, const char* key, const std::string& name, const std::string& where) {
    const json::Value* v = member(obj, key, name, where);
    if (!v->is_string()) invalid(name, std::format("\"{}\" of {} must be a string", key, where));
    return v->as_string();
}

RegistryArchive read_archive(const json::Value& v, const std::string& base, const std::string& name, const std::string& where) {
    if (!v.is_object()) invalid(name, std::format("{} must be an object", where));
    RegistryArchive a;
    a.url = url::join(base, string_member(v, "url", name, where));
    a.integrity = string_member(v, "integrity", name, where);
    if (!valid_integrity(a.integrity)) invalid(name, std::format("{} has an invalid \"integrity\"", where));
    if (const json::Value* s = v.find("stripComponents")) {
        if (!s->is_int() || s->as_int() < 0 || s->as_int() > 16) invalid(name, std::format("{} has an invalid \"stripComponents\"", where));
        a.strip_components = static_cast<int>(s->as_int());
    }
    return a;
}

}  // namespace

RegistryPackage parse_index(std::string_view text, const std::string& name, const std::string& base) {
    json::Value root;
    try {
        root = json::parse(text, json::kStrict);
    } catch (const Error& e) {
        invalid(name, e.what());
    }
    if (!root.is_object()) invalid(name, "the document is not an object");
    if (string_member(root, "name", name, "the document") != name) invalid(name, "\"name\" does not match the file name");
    const json::Value& versions = *member(root, "versions", name, "the document");
    if (!versions.is_object()) invalid(name, "\"versions\" must be an object");

    RegistryPackage pkg;
    pkg.name = name;
    for (const auto& m : versions.as_object()) {
        std::string where = "version " + m.key;
        auto v = semver::Version::parse(m.key);
        if (!v || !v->build.empty()) invalid(name, std::format("\"{}\" is not a valid version", m.key));
        const json::Value& e = m.value;
        if (!e.is_object()) invalid(name, where + " must be an object");
        RegistryVersion rv;
        rv.version = *v;
        rv.source = read_archive(*member(e, "source", name, where), base, name, where + " source");
        auto type = manifest::parse_target_kind(string_member(e, "type", name, where));
        if (!type || *type == manifest::TargetKind::Executable) invalid(name, where + " has an invalid \"type\"");
        rv.type = *type;
        rv.std = string_member(e, "std", name, where);
        const auto& stds = manifest::known_standards();
        if (std::find(stds.begin(), stds.end(), rv.std) == stds.end()) invalid(name, where + " has an invalid \"std\"");
        const json::Value& deps = *member(e, "dependencies", name, where);
        if (!deps.is_object()) invalid(name, where + " \"dependencies\" must be an object");
        for (const auto& d : deps.as_object()) {
            if (!d.value.is_string() || !semver::Range::parse(d.value.as_string()))
                invalid(name, std::format("{} has an invalid range for \"{}\"", where, d.key));
            rv.dependencies.emplace_back(d.key, d.value.as_string());
        }
        if (const json::Value* overlay = e.find("manifest")) {
            if (!overlay->is_object()) invalid(name, where + " \"manifest\" must be an object");
            rv.overlay = *overlay;
        }
        if (const json::Value* bins = e.find("binaries")) {
            if (!bins->is_object()) invalid(name, where + " \"binaries\" must be an object");
            for (const auto& b : bins->as_object()) rv.binaries[b.key] = read_archive(b.value, base, name, where + " binary " + b.key);
        }
        if (const json::Value* y = e.find("yanked")) rv.yanked = y->is_bool() && y->as_bool();
        pkg.versions.push_back(std::move(rv));
    }
    std::sort(pkg.versions.begin(), pkg.versions.end(),
              [](const RegistryVersion& a, const RegistryVersion& b) { return a.version > b.version; });
    return pkg;
}

Registry::Registry(std::string base, bool allow_http, fs::path home, Freshness freshness) :
    base_(url::as_base(base)),
    allow_http_(allow_http),
    home_(std::move(home)),
    cache_(home_ / "cache" / "registry" / Sha256::hex(base_).substr(0, 16)),
    freshness_(freshness) {}

std::optional<std::string> Registry::fetch(const std::string& relative) {
    if (url::is_file(base_)) return fsx::try_read_file(url::to_path(base_) / paths::from_utf8(relative));

    fs::path cached = cache_ / paths::from_utf8(relative);
    std::error_code ec;
    bool have = fs::exists(cached, ec);
    bool fresh = have && fs::file_time_type::clock::now() - fs::last_write_time(cached, ec) < kIndexTtl;
    if (have && (fresh || freshness_ != Freshness::Online)) return fsx::read_file(cached);
    if (freshness_ == Freshness::Offline) {
        throw Error(std::format("{}{} is not cached, and --offline is set", base_, relative));
    }
    fs::path tmp = make_temp_dir(home_, "index");
    try {
        bool found = download(url::join(base_, relative), tmp / "doc.json", allow_http_);
        if (!found) {
            fsx::remove_tree(tmp);
            fs::remove(cached, ec);
            return std::nullopt;
        }
        fsx::create_dirs(cached.parent_path());
        fsx::rename_replace(tmp / "doc.json", cached);
    } catch (const Error& e) {
        fsx::remove_tree(tmp);
        if (!have) throw;
        log::warn(std::format("{}; using the cached copy of {}", e.what(), relative));
        return fsx::read_file(cached);
    }
    fsx::remove_tree(tmp);
    return fsx::read_file(cached);
}

void Registry::check_config() {
    if (config_checked_) return;
    auto text = fetch("config.json");
    if (!text) throw Error(std::format("{} is not a CPM registry (it has no config.json)", base_));
    json::Value cfg;
    try {
        cfg = json::parse(*text, json::kStrict);
    } catch (const Error& e) {
        throw Error(std::format("the config.json of {} is invalid: {}", base_, e.what()));
    }
    const json::Value* api = cfg.find("apiVersion");
    if (api == nullptr || !api->is_int() || api->as_int() != 1) {
        throw Error(std::format("{} uses a registry API version this CPM does not understand", base_), "upgrade CPM");
    }
    config_checked_ = true;
}

const RegistryPackage* Registry::package(const std::string& name) {
    if (auto it = packages_.find(name); it != packages_.end()) return it->second ? &*it->second : nullptr;
    check_config();
    std::optional<RegistryPackage> pkg;
    if (auto text = fetch("index/" + name + ".json")) pkg = parse_index(*text, name, base_);
    auto [it, inserted] = packages_.emplace(name, std::move(pkg));
    return it->second ? &*it->second : nullptr;
}

Registries::Registries(const manifest::UserConfig& config, fs::path home, Freshness freshness) :
    config_(config), home_(std::move(home)), freshness_(freshness) {}

Registry& Registries::make(const std::string& base, bool allow_http) {
    std::string key = url::as_base(base);
    auto it = by_base_.find(key);
    if (it == by_base_.end()) it = by_base_.emplace(key, std::make_unique<Registry>(key, allow_http, home_, freshness_)).first;
    return *it->second;
}

Registry& Registries::get(const std::string& name_or_url) {
    if (name_or_url.find("://") != std::string::npos) {
        bool allow_http = false;
        for (const auto& [n, r] : config_.registries) {
            if (url::as_base(r.url) == url::as_base(name_or_url)) allow_http = r.allow_http;
        }
        return make(name_or_url, allow_http);
    }
    std::string name = name_or_url.empty() ? "default" : name_or_url;
    if (name == "default") {
        if (auto env_url = env::get("CPM_REGISTRY"); env_url && !env_url->empty()) return get(*env_url);
    }
    auto it = config_.registries.find(name);
    if (it == config_.registries.end()) {
        if (name == "default") {
            throw Error("no default registry is configured",
                        "add \"registries\": { \"default\": \"https://...\" } to $CPM_HOME/config.jsonc, or set CPM_REGISTRY");
        }
        throw Error(std::format("unknown registry \"{}\"", name), "registries are configured in $CPM_HOME/config.jsonc");
    }
    return make(it->second.url, it->second.allow_http);
}

Registry& Registries::by_base(const std::string& base) {
    for (const auto& [n, r] : config_.registries) {
        if (url::as_base(r.url) == url::as_base(base)) return make(base, r.allow_http);
    }
    return make(base, false);
}

}  // namespace cpm::install
