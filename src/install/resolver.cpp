#include "install/resolver.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <set>

#include "core/error.hpp"
#include "semver/range.hpp"

namespace cpm::install {

namespace {

using Kind = manifest::DependencySpec::Kind;

// One dependent's demand on a package name.
struct Requirement {
    std::string from;  // "root" or "name@version"
    manifest::DependencySpec spec;
    fs::path declaring_dir;
    bool dev = false;  // a root devDependency
};

std::string describe_requirement(const Requirement& r) {
    std::string what = r.spec.kind == Kind::Registry ? r.spec.range : r.spec.specifier;
    return std::format("{} {}   required by {}", r.spec.name, what, r.from == "root" ? "the project" : r.from);
}

class Solver {
public:
    Solver(Sources& sources, const lock::Lockfile* lock) : sources_(sources), lock_(lock) {}

    void require(Requirement r) { reqs_[r.spec.name].push_back(std::move(r)); }

    bool solve() {
        // The next name to decide: deterministic, by name.
        const std::string* next = nullptr;
        for (const auto& [name, list] : reqs_) {
            if (!list.empty() && !chosen_.contains(name)) {
                next = &name;
                break;
            }
        }
        if (next == nullptr) return true;
        const std::string name = *next;
        const std::vector<Requirement>& list = reqs_[name];

        // The source: the project's own declaration wins; otherwise every dependent must agree.
        const Requirement* source = &list.front();
        for (const auto& r : list) {
            if (r.from == "root") source = &r;
        }
        std::string identity = sources_.identity(source->spec, source->declaring_dir);
        if (source->from != "root") {
            for (const auto& r : list) {
                if (sources_.identity(r.spec, r.declaring_dir) != identity) {
                    throw Error(std::format("\"{}\" is requested from different sources:\n  {}\n  {}", name,
                                            describe_requirement(*source), describe_requirement(r)),
                                std::format("declare \"{}\" in the project's own cpm.jsonc to choose one", name));
                }
            }
        }

        const lock::Package* locked = lock_ ? lock_->find(name) : nullptr;
        std::vector<semver::Version> versions = sources_.candidates(source->spec, source->declaring_dir, locked);
        std::string available;
        for (std::size_t i = 0; i < versions.size() && i < 12; ++i) available += (i ? ", " : "") + versions[i].to_string();
        if (versions.size() > 12) available += ", ...";

        for (const semver::Version& version : versions) {
            // Ranges first (cheap); only versions that fit them are described.
            if (!std::all_of(list.begin(), list.end(), [&](const Requirement& r) { return satisfies(r, version); })) continue;
            const PackageMeta* c = &sources_.describe(source->spec, source->declaring_dir, version, locked);
            if (auto conflict = conflicting_dependency(*c)) {
                // Report the package the constraints disagree on, with every demand on it.
                std::vector<Requirement> all = reqs_.at(conflict->spec.name);
                all.push_back(*conflict);
                if (!failure_) {
                    failure_ = Failure{conflict->spec.name, all, chosen_.at(conflict->spec.name)->version.to_string()};
                }
                continue;
            }
            chosen_.emplace(name, c);
            chosen_identity_[name] = identity;
            std::size_t added = add_requirements(*c);
            if (solve()) return true;
            remove_requirements(*c, added);
            chosen_.erase(name);
            chosen_identity_.erase(name);
        }
        if (!failure_) failure_ = Failure{name, list, available};
        return false;
    }

    [[noreturn]] void report() const {
        std::string msg = "no version of \"" + failure_->name + "\" satisfies every constraint:";
        for (const auto& r : failure_->requirements) msg += "\n  " + describe_requirement(r);
        if (!failure_->available.empty()) msg += "\n  available: " + failure_->available;
        throw Error(msg, "relax one of these constraints, or change the source so a matching version is available");
    }

    Resolution result(const manifest::Manifest& root) const {
        Resolution res;
        for (const auto& [name, meta] : chosen_) {
            ResolvedPackage p;
            p.meta = *meta;
            for (const auto& r : reqs_.at(name)) p.dependents.push_back(r.from);
            res.packages.emplace(name, std::move(p));
        }
        // dev: not reachable from the root's regular dependencies
        std::set<std::string> regular;
        std::function<void(const std::string&)> mark = [&](const std::string& name) {
            if (!regular.insert(name).second) return;
            for (const auto& d : res.packages.at(name).meta.manifest.dependencies) mark(d.name);
        };
        for (const auto& d : root.dependencies) mark(d.name);
        for (auto& [name, p] : res.packages) p.dev = !regular.contains(name);

        // Dependency order (Kahn): a package comes out once everything that depends on it has.
        std::map<std::string, int> dependents;
        for (const auto& [name, p] : res.packages) dependents.try_emplace(name, 0);
        for (const auto& [name, p] : res.packages) {
            for (const auto& d : p.meta.manifest.dependencies) ++dependents[d.name];
        }
        std::set<std::string> ready;
        for (const auto& [name, n] : dependents) {
            if (n == 0) ready.insert(name);
        }
        while (!ready.empty()) {
            std::string name = *ready.begin();
            ready.erase(ready.begin());
            res.order.push_back(name);
            for (const auto& d : res.packages.at(name).meta.manifest.dependencies) {
                if (--dependents[d.name] == 0) ready.insert(d.name);
            }
        }
        if (res.order.size() != res.packages.size()) {
            std::vector<std::string> cycle;
            for (const auto& [name, n] : dependents) {
                if (n > 0) cycle.push_back(name);
            }
            throw Error(std::format("dependency cycle between: {}", [&] {
                std::string s;
                for (const auto& c : cycle) s += (s.empty() ? "" : ", ") + c;
                return s;
            }()));
        }
        return res;
    }

private:
    struct Failure {
        std::string name;
        std::vector<Requirement> requirements;
        std::string available;
    };

    static bool satisfies(const Requirement& r, const semver::Version& v) {
        if (r.spec.kind != Kind::Registry) return true;  // git and path specs constrain the source, not the version
        auto range = semver::Range::parse(r.spec.range);
        return range && range->contains(v);
    }

    // The first dependency of `c` that an already chosen package cannot satisfy. Throws when `c` wants it
    // from a different source than the one chosen (and the project did not pick the source itself).
    std::optional<Requirement> conflicting_dependency(const PackageMeta& c) const {
        for (const auto& d : c.manifest.dependencies) {
            auto it = chosen_.find(d.name);
            if (it == chosen_.end()) continue;
            Requirement r{c.name + "@" + c.version.to_string(), d, c.dir, false};
            const auto& existing = reqs_.at(d.name);
            bool root_chose = std::any_of(existing.begin(), existing.end(), [](const Requirement& e) { return e.from == "root"; });
            if (!root_chose && sources_.identity(d, c.dir) != chosen_identity_.at(d.name)) {
                throw Error(std::format("\"{}\" is requested from different sources:\n  {}\n  {}", d.name,
                                        describe_requirement(existing.front()), describe_requirement(r)),
                            std::format("declare \"{}\" in the project's own cpm.jsonc to choose one", d.name));
            }
            if (!satisfies(r, it->second->version)) return r;
        }
        return std::nullopt;
    }

    std::size_t add_requirements(const PackageMeta& c) {
        std::string from = c.name + "@" + c.version.to_string();
        // Path specs inside a path package are relative to that package's directory.
        for (const auto& d : c.manifest.dependencies) require({from, d, c.dir, false});
        return c.manifest.dependencies.size();
    }

    void remove_requirements(const PackageMeta& c, std::size_t count) {
        std::string from = c.name + "@" + c.version.to_string();
        for (const auto& d : c.manifest.dependencies) {
            auto& list = reqs_[d.name];
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                if (it->from == from) {
                    list.erase(std::next(it).base());
                    break;
                }
            }
        }
        (void)count;
    }

    Sources& sources_;
    const lock::Lockfile* lock_;
    std::map<std::string, std::vector<Requirement>> reqs_;
    std::map<std::string, const PackageMeta*> chosen_;
    std::map<std::string, std::string> chosen_identity_;  // the source each chosen package came from
    std::optional<Failure> failure_;
};

// A dependency must not need a newer C++ standard than the project compiles with (manifest.md §3.2).
void check_std(const manifest::Manifest& root, const Resolution& res) {
    const auto& stds = manifest::known_standards();
    auto rank = [&](const std::string& s) { return std::find(stds.begin(), stds.end(), s) - stds.begin(); };
    for (const auto& [name, p] : res.packages) {
        if (rank(p.meta.manifest.std) > rank(root.std)) {
            throw Error(std::format("{} requires {}, but the project uses {}", name, p.meta.manifest.std, root.std),
                        "raise \"std\" in the project's cpm.jsonc");
        }
        if (p.meta.manifest.type == manifest::TargetKind::SharedLib) {
            throw Error(std::format("{} is a shared library; shared-library dependencies are not supported yet", name),
                        "use it as a static library (\"type\": \"static-lib\" in a manifest overlay)");
        }
    }
}

}  // namespace

Resolution resolve(const manifest::Manifest& root, Sources& sources, const lock::Lockfile* lock) {
    Solver solver(sources, lock);
    for (const auto& d : root.dependencies) solver.require({"root", d, root.root, false});
    for (const auto& d : root.dev_dependencies) solver.require({"root", d, root.root, true});
    if (!solver.solve()) solver.report();
    Resolution res = solver.result(root);
    check_std(root, res);
    return res;
}

}  // namespace cpm::install
