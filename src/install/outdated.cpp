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

}  // namespace cpm::install
