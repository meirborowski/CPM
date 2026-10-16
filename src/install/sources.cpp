#include "install/sources.hpp"

#include <format>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "install/fetch.hpp"
#include "install/git.hpp"
#include "json/write.hpp"

namespace cpm::install {

using Kind = manifest::DependencySpec::Kind;

std::string Sources::identity(const manifest::DependencySpec& spec, const fs::path& declaring_dir) {
    switch (spec.kind) {
        case Kind::Path:
            return "path:" + paths::to_utf8((declaring_dir / paths::from_utf8(spec.path)).lexically_normal());
        case Kind::Git: return "git:" + spec.git_url + "#" + spec.git_ref;
        case Kind::Registry: return "registry:" + registries_.get(spec.registry).base();
    }
    return "?";
}

namespace {

std::optional<std::string> git_pin(const manifest::DependencySpec& spec, const lock::Package* locked) {
    if (locked == nullptr) return std::nullopt;
    const auto* git = std::get_if<lock::GitResolution>(&locked->resolution);
    if (git == nullptr || git->url != spec.git_url || git->ref != spec.git_ref) return std::nullopt;
    return git->commit;
}

void check_kind(const PackageMeta& meta) {
    if (meta.manifest.type == manifest::TargetKind::Executable) {
        throw Error(std::format("{} is an executable; only libraries can be dependencies", meta.name));
    }
}

}  // namespace

const PackageMeta& Sources::describe_single(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                            const lock::Package* locked) {
    std::optional<std::string> pin = spec.kind == Kind::Git ? git_pin(spec, locked) : std::nullopt;
    std::string key = identity(spec, declaring_dir) + "@" + pin.value_or("");
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    PackageMeta meta = spec.kind == Kind::Path ? describe_path(spec, declaring_dir) : describe_git(spec, pin);
    check_kind(meta);
    meta.overlay = spec.overlay;
    return cache_.emplace(key, std::move(meta)).first->second;
}

std::vector<semver::Version> Sources::candidates(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                                 const lock::Package* locked) {
    if (spec.kind != Kind::Registry) return {describe_single(spec, declaring_dir, locked).version};

    Registry& registry = registries_.get(spec.registry);
    const RegistryPackage* pkg = registry.package(spec.name);
    if (pkg == nullptr) {
        throw Error(std::format("the registry {} has no package \"{}\"", registry.base(), spec.name),
                    "check the name, or the registry configured in $CPM_HOME/config.jsonc");
    }
    const auto* locked_reg = locked ? std::get_if<lock::RegistryResolution>(&locked->resolution) : nullptr;
    std::optional<semver::Version> preferred;
    if (locked_reg && url::as_base(locked_reg->registry) == registry.base()) preferred = locked->version;

    std::vector<semver::Version> out;
    if (preferred && pkg->find(*preferred)) out.push_back(*preferred);  // the lockfile pins it, even if yanked
    // Released versions highest first, then prereleases (dependencies.md §4.2); yanked ones are skipped.
    for (bool prerelease : {false, true}) {
        for (const auto& rv : pkg->versions) {
            if (rv.yanked || rv.version.is_prerelease() != prerelease || (preferred && rv.version == *preferred)) continue;
            out.push_back(rv.version);
        }
    }
    return out;
}

const PackageMeta& Sources::describe(const manifest::DependencySpec& spec, const fs::path& declaring_dir,
                                     const semver::Version& version, const lock::Package* locked) {
    if (spec.kind != Kind::Registry) return describe_single(spec, declaring_dir, locked);
    Registry& registry = registries_.get(spec.registry);
    std::string key = "registry:" + registry.base() + spec.name + "@" + version.to_string();
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    const RegistryVersion* rv = registry.package(spec.name)->find(version);
    if (rv == nullptr) throw Error(std::format("internal error: {}@{} vanished from the index", spec.name, version.to_string()));
    PackageMeta meta = describe_registry(spec, registry, *rv);
    return cache_.emplace(key, std::move(meta)).first->second;
}

// Resolution uses the index alone (registry.md §3): the synthesized manifest carries the version,
// type, std and dependencies, which name the same registry. The archive's own cpm.jsonc is read once the
// package is in the store.
PackageMeta Sources::describe_registry(const manifest::DependencySpec& spec, Registry& registry, const RegistryVersion& rv) {
    json::Value doc = json::Value::object();
    doc.set("name", spec.name);
    doc.set("version", rv.version.to_string());
    doc.set("std", rv.std);
    doc.set("type", std::string(manifest::to_string(rv.type)));
    json::Value deps = json::Value::object();
    for (const auto& [name, range] : rv.dependencies) {
        json::Value d = json::Value::object();
        d.set("version", range);
        d.set("registry", registry.base());
        deps.set(name, std::move(d));
    }
    doc.set("dependencies", std::move(deps));
    PackageMeta meta;
    meta.name = spec.name;
    meta.version = rv.version;
    meta.manifest = manifest::parse(json::write(doc), home_ / "cpm.jsonc",
                                    std::format("{}@{} (registry index)", spec.name, rv.version.to_string()));
    meta.resolution = lock::RegistryResolution{registry.base(), rv.source.url, rv.source.integrity};
    meta.overlay = rv.overlay;
    return meta;
}

PackageMeta Sources::describe_path(const manifest::DependencySpec& spec, const fs::path& declaring_dir) {
    fs::path dir = fs::absolute(declaring_dir / paths::from_utf8(spec.path)).lexically_normal();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw Error(std::format("the path dependency \"{}\" points at {}, which is not a directory", spec.name,
                                paths::to_utf8(dir)));
    }
    fs::path file = dir / "cpm.jsonc";
    std::string display = paths::display(file, root_);
    PackageMeta meta;
    meta.name = spec.name;
    meta.manifest = manifest::parse_package(fsx::try_read_file(file), spec.overlay, spec.name, "0.0.0", file, display);
    meta.version = meta.manifest.version;
    meta.resolution = lock::PathResolution{paths::relative_or_absolute(dir, root_)};
    meta.dir = dir;
    return meta;
}

PackageMeta Sources::describe_git(const manifest::DependencySpec& spec, const std::optional<std::string>& pin) {
    auto [mirror, cloned] = git::ensure_mirror(home_, spec.git_url, offline_);
    if (cloned) fetched_.insert(spec.git_url);

    std::optional<std::string> commit;
    if (pin && git::resolve(mirror, *pin) == pin) commit = pin;  // the locked commit is available locally
    if (!commit) {
        // Tags and commits rarely move; branches do, so look them up after a fetch.
        commit = git::resolve(mirror, spec.git_ref);
        bool is_commit = spec.git_ref.size() >= 7 && commit && commit->starts_with(spec.git_ref);
        bool is_tag = git::resolve(mirror, "refs/tags/" + spec.git_ref).has_value() && !spec.git_ref.empty();
        if ((!commit || (!is_commit && !is_tag)) && !offline_ && !fetched_.contains(spec.git_url)) {
            git::fetch(home_, spec.git_url);
            fetched_.insert(spec.git_url);
            commit = git::resolve(mirror, spec.git_ref);
        }
    }
    if (!commit) {
        throw Error(std::format("{} has no ref \"{}\"", spec.git_url, spec.git_ref.empty() ? "HEAD" : spec.git_ref),
                    offline_ ? "--offline is set, so the repository was not fetched" : "check the tag, branch or commit");
    }

    std::string fallback;
    if (auto overlay_version = spec.overlay ? spec.overlay->find("version") : nullptr; overlay_version) {
        fallback = overlay_version->is_string() ? overlay_version->as_string() : "0.0.0";
    } else if (auto tag = git::version_from_tag(spec.git_ref)) {
        fallback = *tag;
    } else {
        fallback = std::format("0.0.0-git.{}", commit->substr(0, 12));
    }
    std::string display = std::format("{} at {}", spec.git_url, commit->substr(0, 12));
    PackageMeta meta;
    meta.name = spec.name;
    meta.manifest = manifest::parse_package(git::show_file(mirror, *commit, "cpm.jsonc"), spec.overlay, spec.name,
                                            fallback, mirror / "cpm.jsonc", display);
    meta.version = meta.manifest.version;
    meta.resolution = lock::GitResolution{spec.git_url, spec.git_ref, *commit};
    meta.dir = mirror;
    for (const auto& d : meta.manifest.dependencies) {
        if (d.kind == Kind::Path) {
            throw Error(std::format("{} depends on the local path {}, which only works inside its own repository",
                                    spec.name, d.specifier),
                        "packages from git may only depend on git or registry packages");
        }
    }
    return meta;
}

}  // namespace cpm::install
