#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "json/value.hpp"
#include "manifest/config.hpp"
#include "manifest/manifest.hpp"
#include "semver/version.hpp"

// Static registries (docs/spec/registry.md): a base URL with config.json and index/<name>.json.
namespace cpm::install {

namespace fs = std::filesystem;

struct RegistryArchive {
    std::string url;        // absolute
    std::string integrity;  // sha256:<hex>
    int strip_components = 0;
};

struct RegistryVersion {
    semver::Version version;
    RegistryArchive source;
    manifest::TargetKind type = manifest::TargetKind::StaticLib;
    std::string std;
    std::vector<std::pair<std::string, std::string>> dependencies;  // name -> range
    std::optional<json::Value> overlay;
    std::map<std::string, RegistryArchive> binaries;               // abi16 -> archive
    bool yanked = false;
};

struct RegistryPackage {
    std::string name;
    std::vector<RegistryVersion> versions;  // highest first

    const RegistryVersion* find(const semver::Version& v) const;
};

// How much index data may come from the cache (registry.md §5).
enum class Freshness { Online, PreferOffline, Offline };

class Registry {
public:
    Registry(std::string base, bool allow_http, fs::path home, Freshness freshness);

    const std::string& base() const { return base_; }
    bool allow_http() const { return allow_http_; }

    // The package's index document, or nullptr if the registry has no such package. Fetched at most once.
    const RegistryPackage* package(const std::string& name);

    // The raw text of index/<name>.json (nullopt if the registry has no such package), after checking the
    // registry's config.json. For `cpm publish`, which adds a version to it.
    std::optional<std::string> index_text(const std::string& name);

    // The base URL of a registry service's API (config.json "api", registry.md §7), with a trailing '/'; nullopt
    // for a static registry.
    const std::optional<std::string>& api();

private:
    std::optional<std::string> fetch(const std::string& relative);
    void check_config();

    std::string base_;
    bool allow_http_;
    fs::path home_, cache_;
    Freshness freshness_;
    bool config_checked_ = false;
    std::optional<std::string> api_;
    std::map<std::string, std::optional<RegistryPackage>> packages_;
};

// The registries a run may use: named ones from config.jsonc ("default" can be overridden with
// CPM_REGISTRY), or a URL given directly.
class Registries {
public:
    Registries(const manifest::UserConfig& config, fs::path home, Freshness freshness);

    // "" means "default".
    Registry& get(const std::string& name_or_url);
    // The registry whose base URL this is (as recorded in cpm-lock.json).
    Registry& by_base(const std::string& base);

    Freshness freshness() const { return freshness_; }

private:
    Registry& make(const std::string& base, bool allow_http);

    manifest::UserConfig config_;
    fs::path home_;
    Freshness freshness_;
    std::map<std::string, std::unique_ptr<Registry>> by_base_;
};

// Parses an index document; throws cpm::Error describing what is wrong with it.
RegistryPackage parse_index(std::string_view text, const std::string& name, const std::string& base);

}  // namespace cpm::install
