#include "install/outdated.hpp"

#include <format>

#include "core/error.hpp"

namespace cpm::install {

bool OutdatedEntry::outdated() const { return current != wanted || current != latest; }

std::optional<semver::Version> newest(const RegistryPackage& package, const semver::Range* range) {
    for (bool prerelease : {false, true}) {
        for (const auto& rv : package.versions) {  // highest first
            if (rv.yanked || rv.version.is_prerelease() != prerelease) continue;
            if (range == nullptr || range->contains(rv.version)) return rv.version;
        }
    }
    return std::nullopt;
}

OutdatedReport outdated(const manifest::Manifest& manifest, const lock::Lockfile* lock, Registries& registries) {
    OutdatedReport report;
    for (const auto* list : {&manifest.dependencies, &manifest.dev_dependencies}) {
        for (const auto& spec : *list) {
            if (spec.kind != manifest::DependencySpec::Kind::Registry) {
                report.skipped.push_back(&spec);
                continue;
            }
            Registry& registry = registries.get(spec.registry);
            const RegistryPackage* pkg = registry.package(spec.name);
            if (pkg == nullptr) {
                throw Error(std::format("the registry {} has no package \"{}\"", registry.base(), spec.name),
                            "check the name, or the registry configured in $CPM_HOME/config.jsonc");
            }
            OutdatedEntry e{.name = spec.name, .dev = spec.dev};
            if (lock) {
                const auto& locked = spec.dev ? lock->dev_dependencies : lock->dependencies;
                if (auto it = locked.find(spec.name); it != locked.end()) e.current = it->second.version;
            }
            if (auto range = semver::Range::parse(spec.range)) e.wanted = newest(*pkg, &*range);
            e.latest = newest(*pkg, nullptr);
            report.entries.push_back(std::move(e));
        }
    }
    return report;
}

std::vector<std::string> unneeded_overrides(const manifest::Manifest& manifest, const lock::Lockfile& lock) {
    std::vector<std::string> notes;
    for (const auto& [key, range] : manifest.overrides) {
        auto arrow = key.find('>');
        std::string name = arrow == std::string::npos ? key : key.substr(arrow + 1);
        // The requirements this override replaced: the lockfile records each with the dependent's own range.
        std::vector<std::string> dependents;
        bool all_allow = true;
        const lock::Package* target = lock.find(name);
        for (const auto& [k, p] : lock.packages) {
            auto own = p.overridden.find(name);
            if (own == p.overridden.end()) continue;
            const auto* applied = manifest::find_override(manifest.overrides, p.name, name);
            if (applied == nullptr || applied->first != key) continue;
            dependents.push_back(p.name);
            auto r = semver::Range::parse(own->second);
            if (!target || !r || !r->contains(target->version)) all_allow = false;
        }
        if (dependents.empty()) {
            notes.push_back(std::format("the override \"{}\" replaces no requirement; remove it from cpm.jsonc", key));
        } else if (all_allow) {
            std::string who;
            for (std::size_t i = 0; i < dependents.size(); ++i) {
                who += (i == 0 ? "" : i + 1 == dependents.size() ? " and " : ", ") + dependents[i];
            }
            notes.push_back(std::format("the override \"{}\" is no longer needed: {} accept{} {} {} by {}", key, who,
                                        dependents.size() == 1 ? "s" : "", name, target->version.to_string(),
                                        dependents.size() == 1 ? "itself" : "themselves"));
        }
    }
    return notes;
}

}  // namespace cpm::install
