#include "install/installer.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <set>

#include "build/layout.hpp"
#include "build/runner.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "install/cache.hpp"
#include "install/dep_builder.hpp"
#include "install/git.hpp"
#include "install/fetch.hpp"
#include "install/linker.hpp"
#include "install/registry.hpp"
#include "install/resolver.hpp"
#include "install/sources.hpp"
#include "install/store.hpp"
#include "json/edit.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "lock/lockfile.hpp"
#include "semver/range.hpp"

namespace cpm::install {

namespace {

namespace fs = std::filesystem;
using Kind = manifest::DependencySpec::Kind;
using manifest::TargetKind;

std::optional<fsx::LinkKind> link_mode(const manifest::UserConfig& config) {
    if (config.link_mode == "symlink") return fsx::LinkKind::Symlink;
    if (config.link_mode == "junction") return fsx::LinkKind::Junction;
    if (config.link_mode == "copy") return fsx::LinkKind::Copy;
    return std::nullopt;
}

std::string lock_hash(const lock::Lockfile& lock) { return "sha256:" + Sha256::hex(lock::serialize(lock)); }

const manifest::DependencySpec* root_spec(const manifest::Manifest& root, const std::string& name) {
    for (const auto* list : {&root.dependencies, &root.dev_dependencies}) {
        for (const auto& d : *list) {
            if (d.name == name) return &d;
        }
    }
    return nullptr;
}

manifest::Manifest path_manifest(const fs::path& root_dir, const manifest::Manifest& root, const lock::Package& p) {
    fs::path dir = (root_dir / paths::from_utf8(std::get<lock::PathResolution>(p.resolution).path)).lexically_normal();
    fs::path file = dir / "cpm.jsonc";
    const manifest::DependencySpec* spec = root_spec(root, p.name);
    std::optional<json::Value> overlay = spec ? spec->overlay : std::nullopt;
    return manifest::parse_package(fsx::try_read_file(file), overlay, p.name, "0.0.0", file, paths::display(file, root_dir));
}

// Lock packages in dependency order: every package before the packages it depends on.
std::vector<const lock::Package*> dependency_order(const lock::Lockfile& lock) {
    std::map<std::string, const lock::Package*> by_name;
    std::map<std::string, int> dependents;
    for (const auto& [key, p] : lock.packages) {
        by_name[p.name] = &p;
        dependents.try_emplace(p.name, 0);
    }
    for (const auto& [key, p] : lock.packages) {
        for (const auto& [dep, v] : p.dependencies) ++dependents[dep];
    }
    std::set<std::string> ready;
    for (const auto& [name, n] : dependents) {
        if (n == 0) ready.insert(name);
    }
    std::vector<const lock::Package*> order;
    while (!ready.empty()) {
        std::string name = *ready.begin();
        ready.erase(ready.begin());
        const lock::Package* p = by_name.at(name);
        order.push_back(p);
        for (const auto& [dep, v] : p->dependencies) {
            if (--dependents[dep] == 0) ready.insert(dep);
        }
    }
    if (order.size() != by_name.size()) throw Error("cpm-lock.json contains a dependency cycle");
    return order;
}

// --- turning a lockfile into store entries, libraries, links and build inputs -----------------------

struct PackageInfo {
    manifest::Manifest manifest;
    fs::path dir;  // store entry or path package directory
    build::Layout layout;
    std::optional<fs::path> include;         // absolute public include directory
    std::optional<fs::path> lib;             // absolute store lib/<abi16>
    std::vector<std::string> deps_includes;  // names of all (transitive) dependencies, for include paths
    std::vector<std::string> trans_defines;  // public definitions of all (transitive) dependencies
};

struct Materialized {
    std::vector<build::Dependency> dependencies;
    std::vector<LinkTarget> links;
    bool complete = true;  // false when --no-build left a library unbuilt
};

// The index entry of a locked registry package.
const RegistryVersion& index_entry(Registries& registries, const lock::Package& p) {
    const auto& reg = std::get<lock::RegistryResolution>(p.resolution);
    const RegistryPackage* pkg = registries.by_base(reg.registry).package(p.name);
    const RegistryVersion* rv = pkg ? pkg->find(p.version) : nullptr;
    if (rv == nullptr) {
        throw Error(std::format("the registry {} no longer lists {}", reg.registry, p.key()),
                    "registries must not remove published versions; ask its maintainers, or pick another version");
    }
    if (rv->source.integrity != reg.integrity) {
        throw Error(std::format("the registry now publishes different content for {} than cpm-lock.json recorded", p.key()),
                    "published versions are immutable (registry.md §3); this may be tampering");
    }
    return *rv;
}

Materialized materialize(const build::Project& project, const lock::Lockfile& lock, Registries& registries, bool offline,
                         bool no_build, const manifest::UserConfig& config) {
    const auto order = dependency_order(lock);
    const toolchain::Toolchain& tc = project.toolchain;
    std::map<std::string, PackageInfo> info;
    std::map<std::string, std::optional<json::Value>> overlays;  // from whoever declared the package
    for (const auto* list : {&project.manifest.dependencies, &project.manifest.dev_dependencies}) {
        for (const auto& d : *list) overlays[d.name] = d.overlay;
    }

    // Pass 1, dependents first: extract store entries and read every manifest (a package's manifest
    // supplies the overlays of the packages it declares).
    for (const lock::Package* p : order) {
        PackageInfo i;
        if (std::holds_alternative<lock::GitResolution>(p->resolution)) {
            i.dir = ensure_git_entry(project.home, *p, overlays[p->name], offline);
            i.manifest = entry_manifest(i.dir, *p);
        } else if (std::holds_alternative<lock::PathResolution>(p->resolution)) {
            i.manifest = path_manifest(project.root, project.manifest, *p);
            i.dir = i.manifest.root;
        } else {
            i.dir = entry_dir(project.home, *p);
            if (!entry_intact(i.dir)) {
                const RegistryVersion& rv = index_entry(registries, *p);
                Registry& registry = registries.by_base(std::get<lock::RegistryResolution>(p->resolution).registry);
                i.dir = ensure_registry_entry(project.home, *p, rv.source.strip_components, rv.overlay,
                                              registry.allow_http(), offline);
            }
            i.manifest = entry_manifest(i.dir, *p);
        }
        for (const auto& d : i.manifest.dependencies) overlays.try_emplace(d.name, d.overlay);
        i.layout = build::infer_layout(i.manifest, manifest::effective_platform(i.manifest, tc.os));
        if (i.layout.include_dir_exists) i.include = (i.dir / paths::from_utf8(i.manifest.include_dir)).lexically_normal();
        info.emplace(p->name, std::move(i));
    }

    // Pass 2, dependencies first: what each package sees of its dependencies, and store libraries built.
    Materialized out;
    std::optional<fs::path> ninja;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const lock::Package& p = **it;
        PackageInfo& i = info.at(p.name);
        for (const auto& [dep, v] : p.dependencies) {
            const PackageInfo& d = info.at(dep);
            auto add = [](std::vector<std::string>& to, const std::string& value) {
                if (std::find(to.begin(), to.end(), value) == to.end()) to.push_back(value);
            };
            add(i.deps_includes, dep);
            for (const auto& x : d.deps_includes) add(i.deps_includes, x);
            manifest::PlatformOverrides dp = manifest::effective_platform(d.manifest, tc.os);
            for (const auto* list : std::initializer_list<const std::vector<std::string>*>{&d.manifest.public_defines, &dp.public_defines, &d.trans_defines}) {
                for (const auto& x : *list) add(i.trans_defines, x);
            }
        }
        bool store_library = !std::holds_alternative<lock::PathResolution>(p.resolution) &&
                             i.layout.kind == TargetKind::StaticLib;
        if (store_library) {
            fs::path lib_dir = i.dir / "lib" / project.abi.dir();
            if (library_intact(lib_dir)) {
                i.lib = lib_dir;
            } else if (const auto* reg = std::get_if<lock::RegistryResolution>(&p.resolution)) {
                // A prebuilt binary for this ABI saves the build (registry.md §4.2).
                const RegistryVersion& rv = index_entry(registries, p);
                auto bin = rv.binaries.find(project.abi.dir());
                if (bin != rv.binaries.end() &&
                    install_prebuilt(project, p, i.dir, bin->second.url, bin->second.integrity,
                                     registries.by_base(reg->registry).allow_http(), offline))
                    i.lib = lib_dir;
            }
            if (!i.lib) {
                if (no_build) {
                    out.complete = false;
                } else {
                    DependencyInputs inputs;
                    for (const auto& dep : i.deps_includes) {
                        if (info.at(dep).include) inputs.include_dirs.push_back(*info.at(dep).include);
                    }
                    inputs.public_defines = i.trans_defines;
                    if (!ninja) ninja = build::find_ninja(config.ninja, project.home);
                    i.lib = ensure_library(project, p, i.dir, i.manifest, inputs, *ninja);
                }
            }
        }
    }

    // Pass 3, dependency order: build inputs and links.
    for (const lock::Package* p : order) {
        const PackageInfo& i = info.at(p->name);
        manifest::PlatformOverrides platform = manifest::effective_platform(i.manifest, tc.os);
        build::Dependency d;
        d.name = p->name;
        d.version = p->version.to_string();
        d.dev = p->dev;
        if (const auto* reg = std::get_if<lock::RegistryResolution>(&p->resolution)) d.identity = reg->integrity;
        else if (const auto* git = std::get_if<lock::GitResolution>(&p->resolution)) d.identity = "git:" + git->commit;
        else d.identity = "path:" + std::get<lock::PathResolution>(p->resolution).path;
        if (i.include) d.include_dir = std::format("cpm_modules/{}/include", p->name);
        d.public_defines = i.manifest.public_defines;
        d.public_defines.insert(d.public_defines.end(), platform.public_defines.begin(), platform.public_defines.end());
        d.system_libs = i.manifest.system_libs;
        d.system_libs.insert(d.system_libs.end(), platform.system_libs.begin(), platform.system_libs.end());
        if (i.lib) {
            d.lib_dir = std::format("cpm_modules/{}/lib", p->name);
            d.link_name = p->name;
            d.lib_file = d.lib_dir + "/" + project.driver->static_lib_file(p->name);
        } else if (std::holds_alternative<lock::PathResolution>(p->resolution) && i.layout.kind == TargetKind::StaticLib) {
            build::Dependency::Sources s;
            s.root = paths::relative_or_absolute(i.dir, project.root);
            s.files = i.layout.sources;
            s.include_dirs.push_back(s.root + "/src");
            if (i.include) s.include_dirs.push_back(paths::relative_or_absolute(*i.include, project.root));
            for (const auto& dep : i.deps_includes) {
                if (info.at(dep).include) s.include_dirs.push_back(std::format("cpm_modules/{}/include", dep));
            }
            for (const auto* list : std::initializer_list<const std::vector<std::string>*>{&i.manifest.defines, &i.manifest.public_defines, &platform.defines,
                                     &platform.public_defines, &i.trans_defines})
                s.defines.insert(s.defines.end(), list->begin(), list->end());
            d.sources = std::move(s);
        }
        out.dependencies.push_back(std::move(d));
        out.links.push_back({p->name, p->version.to_string(), i.include, i.lib});
    }
    return out;
}

// --- the lockfile a resolution produces --------------------------------------------------------------

lock::Lockfile make_lock(const manifest::Manifest& root, const Resolution& res, const build::Project& project, bool offline) {
    lock::Lockfile lock;
    auto root_dep = [&](const manifest::DependencySpec& s) {
        return lock::RootDependency{s.specifier, res.packages.at(s.name).meta.version};
    };
    for (const auto& s : root.dependencies) lock.dependencies[s.name] = root_dep(s);
    for (const auto& s : root.dev_dependencies) lock.dev_dependencies[s.name] = root_dep(s);

    for (const auto& [name, r] : res.packages) {
        lock::Package p;
        p.name = name;
        p.version = r.meta.version;
        p.resolution = r.meta.resolution;
        p.std = r.meta.manifest.std;
        p.dev = r.dev;
        for (const auto& d : r.meta.manifest.dependencies) p.dependencies[d.name] = res.packages.at(d.name).meta.version;
        // The type may be inferred from the files, which for git packages are in the store.
        if (std::holds_alternative<lock::RegistryResolution>(p.resolution)) {
            p.type = r.meta.manifest.type.value_or(TargetKind::StaticLib);  // the index states it
        } else {
            manifest::Manifest m = r.meta.manifest;
            if (std::holds_alternative<lock::GitResolution>(p.resolution)) {
                fs::path entry = ensure_git_entry(project.home, p, r.meta.overlay, offline);
                m = entry_manifest(entry, p);
            }
            p.type = build::infer_layout(m, manifest::effective_platform(m, project.toolchain.os)).kind;
        }
        lock.packages.emplace(p.key(), std::move(p));
    }
    return lock;
}

// --- `cpm install <spec>` --------------------------------------------------------------------------

struct Addition {
    std::string name;
    json::Value value;  // the specifier written into cpm.jsonc
};

bool looks_like_path(std::string_view s) {
    return s.starts_with("./") || s.starts_with("../") || s.starts_with(".\\") || s.starts_with("..\\") ||
           s.starts_with('/') || s == "." || s == ".." || (s.size() >= 2 && s[1] == ':');
}

std::string name_from_git(const build::Project& project, const std::string& specifier, bool offline) {
    // Parse the specifier the way the manifest would, then read the package's own name at that ref.
    std::string probe = std::format(R"({{"name": "probe", "version": "0.0.0", "dependencies": {{"x": {}}}}})",
                                    json::quote(specifier));
    manifest::Manifest m = manifest::parse(probe, project.root / "cpm.jsonc", "the command line");
    const manifest::DependencySpec& spec = m.dependencies.front();
    auto [mirror, cloned] = git::ensure_mirror(project.home, spec.git_url, offline);
    auto commit = git::resolve(mirror, spec.git_ref);
    if (!commit && !offline && !cloned) {
        git::fetch(project.home, spec.git_url);
        commit = git::resolve(mirror, spec.git_ref);
    }
    if (!commit) throw Error(std::format("{} has no ref \"{}\"", spec.git_url, spec.git_ref));
    auto text = git::show_file(mirror, *commit, "cpm.jsonc");
    if (!text) {
        throw Error(std::format("{} has no cpm.jsonc, so its name and build settings are unknown", spec.git_url),
                    std::format("add it to cpm.jsonc by hand with a manifest overlay, e.g. \"name\": {{ \"git\": \"{}\", "
                                "\"manifest\": {{ \"type\": \"header-only\" }} }}",
                                spec.git_url));
    }
    json::Value v = json::parse(*text);
    const json::Value* n = v.find("name");
    if (n == nullptr || !n->is_string()) throw Error(std::format("the cpm.jsonc of {} has no \"name\"", spec.git_url));
    return n->as_string();
}

Addition parse_addition(const std::string& arg, const build::Project& project, const fs::path& cwd, const Options& options,
                        Registries& registries) {
    const bool offline = options.offline;
    std::optional<std::string> name;
    std::string rest = arg;
    bool prefixed = arg.starts_with("file:") || arg.starts_with("git+") || arg.starts_with("github:");
    if (!prefixed && !looks_like_path(arg)) {
        auto at = arg.find('@');
        if (at != std::string::npos && at > 0) {
            name = arg.substr(0, at);
            rest = arg.substr(at + 1);
        } else {
            std::error_code ec;
            if (!fs::is_directory(cwd / paths::from_utf8(arg), ec)) name = arg, rest.clear();
        }
    }
    if (name && !manifest::valid_package_name(*name)) throw UsageError(std::format("invalid package name '{}'", *name));

    if (rest.starts_with("git+") || rest.starts_with("github:")) {
        return {name ? *name : name_from_git(project, rest, offline), json::Value(rest)};
    }
    if (rest.starts_with("file:") || looks_like_path(rest) || (!rest.empty() && !name)) {
        std::string p = rest.starts_with("file:") ? rest.substr(5) : rest;
        fs::path dir = fs::absolute(cwd / paths::from_utf8(p)).lexically_normal();
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) throw Error(std::format("{} is not a directory", paths::to_utf8(dir)));
        std::string pkg_name;
        if (name) {
            pkg_name = *name;
        } else {
            if (!fs::exists(dir / "cpm.jsonc", ec))
                throw Error(std::format("{} has no cpm.jsonc", paths::to_utf8(dir)), "give the name: cpm install <name>@file:<path>");
            pkg_name = manifest::load(dir / "cpm.jsonc", cwd).name;
        }
        return {pkg_name, json::Value("file:" + paths::relative_or_absolute(dir, project.root))};
    }

    // A registry package: "fmt" (the newest version, as ^x.y.z or exact with -E) or "fmt@<range>".
    Registry& registry = registries.get(options.registry);
    const RegistryPackage* pkg = registry.package(*name);
    if (pkg == nullptr) {
        throw Error(std::format("the registry {} has no package \"{}\"", registry.base(), *name),
                    "check the name; paths start with ./ or ../, git URLs with git+ or github:");
    }
    std::string range = rest;
    if (range.empty()) {
        auto latest = std::find_if(pkg->versions.begin(), pkg->versions.end(),
                                   [](const RegistryVersion& v) { return !v.yanked && !v.version.is_prerelease(); });
        if (latest == pkg->versions.end()) throw Error(std::format("{} has no released (non-prerelease) version", *name));
        range = (options.exact ? "" : "^") + latest->version.to_string();
    } else if (!semver::Range::parse(range)) {
        throw UsageError(std::format("invalid version range '{}'", range), "use a range such as ^1.2.0, ~1.4 or >=2 <3");
    }
    if (options.registry.empty()) return {*name, json::Value(range)};
    json::Value spec = json::Value::object();
    spec.set("version", range);
    spec.set("registry", options.registry);
    return {*name, std::move(spec)};
}

// --- reporting ------------------------------------------------------------------------------------

std::string source_label(const lock::Resolution& r) {
    if (const auto* git = std::get_if<lock::GitResolution>(&r)) return std::format("git {}", git->commit.substr(0, 12));
    if (std::holds_alternative<lock::PathResolution>(r)) return "path";
    return "registry";  // the version says the rest
}

void report(const std::optional<lock::Lockfile>& before, const lock::Lockfile& after, double seconds) {
    std::map<std::string, const lock::Package*> old_pkgs, new_pkgs;
    if (before) {
        for (const auto& [k, p] : before->packages) old_pkgs[p.name] = &p;
    }
    for (const auto& [k, p] : after.packages) new_pkgs[p.name] = &p;
    std::vector<std::string> lines;
    for (const auto& [name, p] : new_pkgs) {
        std::string dev = p->dev ? ", dev" : "";
        auto it = old_pkgs.find(name);
        if (it == old_pkgs.end()) {
            lines.push_back(std::format("{} {} {} ({}{})", log::green("+"), name, p->version.to_string(),
                                        source_label(p->resolution), dev));
        } else if (it->second->version != p->version || source_label(it->second->resolution) != source_label(p->resolution)) {
            lines.push_back(std::format("{} {} {} -> {} ({}{})", log::yellow("~"), name, it->second->version.to_string(),
                                        p->version.to_string(), source_label(p->resolution), dev));
        }
    }
    for (const auto& [name, p] : old_pkgs) {
        if (!new_pkgs.contains(name)) lines.push_back(std::format("{} {} {}", log::red("-"), name, p->version.to_string()));
    }
    for (const auto& l : lines) log::info(l);
    std::size_t n = after.packages.size();
    log::status("Done", std::format("{}{} package{} in {:.2f}s", lines.empty() ? "already up to date, " : "", n,
                                    n == 1 ? "" : "s", seconds));
}

}  // namespace

std::vector<build::Dependency> install(build::Project& project, const Options& options, const manifest::UserConfig& config) {
    auto start = std::chrono::steady_clock::now();
    const fs::path& root = project.root;
    std::optional<lock::Lockfile> old_lock = lock::load(root, root);
    Registries registries(config, project.home,
                          options.offline          ? Freshness::Offline
                          : options.prefer_offline ? Freshness::PreferOffline
                                                   : Freshness::Online);

    // 1. Manifest changes, applied to the text so comments survive (manifest.md §10).
    std::string text = project.manifest.text;
    std::vector<std::string> changes;  // what happened to cpm.jsonc, for the report
    for (const auto& arg : options.add) {
        Addition a = parse_addition(arg, project, options.cwd.empty() ? project.root : options.cwd, options, registries);
        if (a.name == project.manifest.name) throw Error(std::format("a package cannot depend on itself ({})", a.name));
        const char* section = options.dev ? "devDependencies" : "dependencies";
        const char* other = options.dev ? "dependencies" : "devDependencies";
        std::string before = text;
        text = json::edit::set_member(text, {section, a.name}, a.value);
        text = json::edit::remove_member(text, {other, a.name});
        if (text != before) {
            changes.push_back(std::format("{} \"{}\": {}", section, a.name,
                                          json::write(a.value, {.compact = true})));
        }
    }
    for (const auto& name : options.remove) {
        if (!root_spec(project.manifest, name)) {
            throw Error(std::format("\"{}\" is not a dependency of {}", name, project.manifest.name),
                        "see \"dependencies\" and \"devDependencies\" in cpm.jsonc");
        }
        text = json::edit::remove_member(text, {"dependencies", name});
        text = json::edit::remove_member(text, {"devDependencies", name});
        changes.push_back(std::format("removed \"{}\"", name));
    }
    manifest::Manifest m = text == project.manifest.text
                               ? project.manifest
                               : manifest::parse(text, project.manifest.file, "cpm.jsonc");

    // 2. --frozen-lockfile: the lock must already describe the manifest.
    auto read_path = [&](const lock::Package& p) { return path_manifest(root, m, p); };
    if (options.frozen) {
        if (!options.add.empty() || !options.remove.empty())
            throw UsageError("--frozen-lockfile cannot be combined with adding or removing packages");
        if (!old_lock) throw Error("--frozen-lockfile was given, but there is no cpm-lock.json");
        if (auto why = lock::staleness(*old_lock, m, read_path))
            throw Error(std::format("cpm-lock.json is out of date: {}", *why), "run 'cpm install' and commit the result");
    }

    // 3. Resolve and lock. `cpm update` takes away the lock's preferences for the packages it updates, so the
    // solver picks the newest versions the ranges allow (and moves git branches to their current commit).
    std::optional<lock::Lockfile> pins = old_lock;
    if (pins && options.update_all) pins.reset();
    for (const auto& name : options.update) {
        if (!pins || !pins->find(name)) {
            throw Error(std::format("\"{}\" is not installed, so it cannot be updated", name),
                        "run 'cpm install' first, or check the name in cpm-lock.json");
        }
        std::erase_if(pins->packages, [&](const auto& entry) { return entry.second.name == name; });
    }
    Sources sources(root, project.home, registries, options.offline);
    Resolution res = resolve(m, sources, pins ? &*pins : nullptr);
    lock::Lockfile new_lock = make_lock(m, res, project, options.offline);
    if (options.frozen && lock::serialize(new_lock) != lock::serialize(*old_lock)) {
        throw Error("resolving again gives a different result than cpm-lock.json", "run 'cpm install' and commit the result");
    }

    // 4. Store entries, libraries for this ABI, links.
    build::Project view = project;
    view.manifest = m;
    Materialized mat = materialize(view, new_lock, registries, options.offline, options.no_build, config);
    // After --no-build the state stays "not current", so the next build finishes the job.
    link_modules(root, mat.links, mat.complete ? lock_hash(new_lock) : std::string(), project.abi.dir(), link_mode(config));

    // 5. Only now write the manifest and the lockfile.
    if (text != project.manifest.text) {
        fsx::write_file_atomic(project.manifest.file, text);
        build::reload_manifest(project);
    }
    if (!options.frozen) lock::save(root, new_lock);
    cache::register_project(project.home, root);
    for (const auto& c : changes) log::status("Updated", std::format("cpm.jsonc: {}", c));

    report(old_lock, new_lock, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return mat.dependencies;
}

std::vector<build::Dependency> ensure(const build::Project& project, const manifest::UserConfig& config) {
    const manifest::Manifest& m = project.manifest;
    std::optional<lock::Lockfile> lock = lock::load(project.root, project.root);
    if (m.dependencies.empty() && m.dev_dependencies.empty() && (!lock || lock->packages.empty())) return {};

    std::optional<std::string> why;
    if (!lock) {
        why = "there is no cpm-lock.json yet";
    } else {
        why = lock::staleness(*lock, m, [&](const lock::Package& p) { return path_manifest(project.root, m, p); });
    }
    if (why) {
        log::status("Installing", std::format("dependencies ({})", *why));
        build::Project copy = project;
        return install(copy, {}, config);
    }

    Registries registries(config, project.home, Freshness::Online);
    Materialized mat = materialize(project, *lock, registries, false, false, config);
    std::string hash = lock_hash(*lock);
    if (!modules_current(project.root, hash, project.abi.dir())) {
        link_modules(project.root, mat.links, hash, project.abi.dir(), link_mode(config));
    }
    cache::register_project(project.home, project.root);
    return mat.dependencies;
}

manifest::Manifest path_package_manifest(const fs::path& root_dir, const manifest::Manifest& root, const lock::Package& package) {
    return path_manifest(root_dir, root, package);
}

}  // namespace cpm::install
