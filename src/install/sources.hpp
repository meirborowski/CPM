#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "install/registry.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"

namespace cpm::install {

namespace fs = std::filesystem;

// One concrete version of a package, as resolution sees it.
struct PackageMeta {
    std::string name;
    semver::Version version;
    manifest::Manifest manifest;  // effective manifest (overlay applied); for registry packages built from the index
    lock::Resolution resolution;  // where it comes from
    fs::path dir;                 // path packages: the package directory; git: the mirror
    std::optional<json::Value> overlay;  // the manifest overlay it was described with
};

// Turns dependency specs into package metadata (docs/spec/dependencies.md §1, §4.1). Results are cached,
// so each git repository and registry index is fetched at most once per run.
class Sources {
public:
    Sources(fs::path project_root, fs::path home, Registries& registries, bool offline) :
        root_(std::move(project_root)), home_(std::move(home)), registries_(registries), offline_(offline) {}

    // The versions a spec can resolve to, best first (§4.2): the locked version, then newest first.
    // Path and git specs have exactly one. `locked` is the lockfile's package of that name, if any.
    std::vector<semver::Version> candidates(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                            const lock::Package* locked);

    // One candidate's metadata.
    const PackageMeta& describe(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                const semver::Version& version, const lock::Package* locked);

    // What makes two specs name the same source: the absolute directory, the git URL and ref, or the
    // registry's base URL.
    std::string identity(const manifest::DependencySpec& spec, const fs::path& declaring_dir);

    Registries& registries() { return registries_; }

private:
    const PackageMeta& describe_single(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                       const lock::Package* locked);
    PackageMeta describe_path(const manifest::DependencySpec& spec, const fs::path& declaring_dir);
    PackageMeta describe_git(const manifest::DependencySpec& spec, const std::optional<std::string>& pin);
    PackageMeta describe_registry(const manifest::DependencySpec& spec, Registry& registry, const RegistryVersion& rv);

    fs::path root_, home_;
    Registries& registries_;
    bool offline_;
    std::map<std::string, PackageMeta> cache_;  // by identity + version/pin
    std::set<std::string> fetched_;             // git mirrors updated in this run
};

}  // namespace cpm::install
