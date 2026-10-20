#include "install/resolver.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "core/error.hpp"
#include "semver/range.hpp"

// PubGrub (docs/spec/dependencies.md §4.2), after Natalie Weizenbaum's description of the algorithm in Dart's pub.
// Version sets are subsets of the versions each package actually has, so every set operation is exact.
namespace cpm::install {

namespace {

using Kind = manifest::DependencySpec::Kind;

constexpr int kProject = 0;  // the package id of the project being resolved

// A set of versions of one package: bit i stands for the package's i-th version, in ascending order.
class Bits {
public:
    Bits() = default;
    explicit Bits(std::size_t size, bool full = false) : size_(size), words_((size + 63) / 64, full ? ~std::uint64_t{0} : 0) {
        trim();
    }

    static Bits only(std::size_t size, std::size_t i) {
        Bits b(size);
        b.set(i);
        return b;
    }

    void set(std::size_t i) { words_[i / 64] |= std::uint64_t{1} << (i % 64); }
    bool test(std::size_t i) const { return (words_[i / 64] >> (i % 64)) & 1; }
    std::size_t size() const { return size_; }
    bool none() const {
        return std::all_of(words_.begin(), words_.end(), [](std::uint64_t w) { return w == 0; });
    }
    std::size_t count() const {
        std::size_t n = 0;
        for (auto w : words_) n += static_cast<std::size_t>(std::popcount(w));
        return n;
    }
    bool subset_of(const Bits& other) const {
        for (std::size_t i = 0; i < words_.size(); ++i) {
            if (words_[i] & ~other.words_[i]) return false;
        }
        return true;
    }
    Bits operator&(const Bits& other) const { return combine(other, [](auto a, auto b) { return a & b; }); }
    Bits operator|(const Bits& other) const { return combine(other, [](auto a, auto b) { return a | b; }); }
    Bits operator~() const {
        Bits out = *this;
        for (auto& w : out.words_) w = ~w;
        out.trim();
        return out;
    }
    bool operator==(const Bits&) const = default;

private:
    template <typename Op>
    Bits combine(const Bits& other, Op op) const {
        Bits out = *this;
        for (std::size_t i = 0; i < words_.size(); ++i) out.words_[i] = op(words_[i], other.words_[i]);
        return out;
    }
    void trim() {
        if (size_ % 64 != 0 && !words_.empty()) words_.back() &= (std::uint64_t{1} << (size_ % 64)) - 1;
    }

    std::size_t size_ = 0;
    std::vector<std::uint64_t> words_;
};

// "package is selected with a version in set" (positive) or "package is not selected with a version in set".
struct Term {
    int pkg = kProject;
    bool positive = true;
    Bits set;
    std::string label;  // the range the set was written as, for explanations; empty when it was computed
};

Term negate(Term t) {
    t.positive = !t.positive;
    return t;
}

Term intersect(const Term& a, const Term& b) {
    Term r{a.pkg, true, {}, {}};
    if (a.positive && b.positive) {
        r.set = a.set & b.set;
    } else if (a.positive) {
        r.set = a.set & ~b.set;
    } else if (b.positive) {
        r.set = b.set & ~a.set;
    } else {
        r.positive = false;
        r.set = a.set | b.set;
    }
    if (!a.label.empty() && r.set == a.set) r.label = a.label;
    else if (!b.label.empty() && r.set == b.set) r.label = b.label;
    return r;
}

// Every selection that `a` allows is allowed by `b`.
bool satisfies(const Term& a, const Term& b) {
    if (a.positive && b.positive) return a.set.subset_of(b.set);
    if (a.positive) return (a.set & b.set).none();
    if (b.positive) return false;
    return b.set.subset_of(a.set);
}

bool is_empty(const Term& t) { return t.positive && t.set.none(); }

// A set of terms that can't all be true (§4.2).
struct Incompatibility {
    enum class Cause { Root, Dependency, NoVersions, Derived };
    std::vector<Term> terms;
    Cause cause = Cause::Root;
    int left = -1, right = -1;   // Derived: the two incompatibilities it was derived from
    std::optional<Term> unmet;   // Dependency: a requirement that matches no version (terms then hold only the dependent)
};
using Cause = Incompatibility::Cause;

// One package name, with the versions it can have.
struct Package {
    std::string name;                     // empty for the project
    manifest::DependencySpec spec;        // the requirement that fixed its source
    fs::path declaring_dir;               // where `spec` was declared (path specs are relative to it)
    std::string from;                     // who declared `spec`: "root" or "name@version"
    bool root_declared = false;
    std::string missing;                  // the registry, when it has no package of this name
    const lock::Package* locked = nullptr;
    std::vector<semver::Version> versions;  // ascending
    std::vector<std::size_t> preference;    // indices into `versions`, best candidate first (§4.2)
    std::vector<int> incompatibilities;     // the ones whose terms mention this package
    std::optional<std::size_t> decided;

    Term all() const { return {0, true, Bits(versions.size(), true), {}}; }
};

struct Assignment {
    Term term;
    int level = 0;
    int cause = -1;  // the incompatibility it was derived from; -1 for a decision
};

std::string describe_requirement(const manifest::DependencySpec& spec, const std::string& from) {
    std::string what = spec.kind == Kind::Registry ? spec.range : spec.specifier;
    return std::format("{} {}   required by {}", spec.name, what, from == "root" ? "the project" : from);
}

// "git <url>#<ref>" or "path <path>"
std::string source_text(const manifest::DependencySpec& spec) {
    if (spec.kind == Kind::Git) return "git " + spec.git_url + (spec.git_ref.empty() ? "" : "#" + spec.git_ref);
    return "path " + spec.path;
}

// "a", "a or b", "a, b or c"
std::string join(const std::vector<std::string>& items, std::string_view last) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += i + 1 == items.size() ? std::format(" {} ", last) : ", ";
        out += items[i];
    }
    return out;
}

class Solver {
public:
    Solver(const manifest::Manifest& root, Sources& sources, const lock::Lockfile* lock) :
        root_(root), sources_(sources), lock_(lock) {
        Package project;
        project.versions = {root.version};
        project.preference = {0};
        packages_.push_back(std::move(project));
        acc_.emplace_back();
        // The project's own declarations fix the sources of the names it lists (§4.1).
        for (const auto& [spec, dir] : dependencies(kProject, 0)) package_for(spec, dir, "root");
    }

    void solve() {
        Term project = packages_[kProject].all();
        project.pkg = kProject;
        attach(create({negate(project)}, Cause::Root));
        int next = kProject;
        while (true) {
            propagate(next);
            auto chosen = choose();
            if (!chosen) return;
            next = *chosen;
        }
    }

    Resolution result() const;

private:
    // --- packages ---------------------------------------------------------------------------------

    std::vector<std::pair<manifest::DependencySpec, fs::path>> dependencies(int pkg, std::size_t version) {
        std::vector<std::pair<manifest::DependencySpec, fs::path>> out;
        if (pkg == kProject) {
            for (const auto* list : {&root_.dependencies, &root_.dev_dependencies}) {
                for (const auto& d : *list) out.emplace_back(d, root_.root);
            }
            return out;
        }
        const PackageMeta& m = meta(pkg, version);
        // Path specs inside a path package are relative to that package's directory.
        for (const auto& d : m.manifest.dependencies) out.emplace_back(d, m.dir);
        return out;
    }

    const PackageMeta& meta(int pkg, std::size_t version) {
        const Package& p = packages_[pkg];
        return sources_.describe(p.spec, p.declaring_dir, p.versions[version], p.locked);
    }

    std::string key(int pkg, std::size_t version) const {
        return packages_[pkg].name + "@" + packages_[pkg].versions[version].to_string();
    }

    // The package a requirement names, created on first sight: the first declaration fixes its source, and
    // later ones must agree unless the project declared it (§4.1).
    int package_for(const manifest::DependencySpec& spec, const fs::path& dir, const std::string& from) {
        if (auto it = by_name_.find(spec.name); it != by_name_.end()) {
            const Package& p = packages_[it->second];
            if (!p.root_declared && sources_.identity(spec, dir) != identity_[it->second]) {
                throw Error(std::format("\"{}\" is requested from different sources:\n  {}\n  {}", spec.name,
                                        describe_requirement(p.spec, p.from), describe_requirement(spec, from)),
                            std::format("declare \"{}\" in the project's own cpm.jsonc to choose one", spec.name));
            }
            return it->second;
        }
        Package p;
        p.name = spec.name;
        p.spec = spec;
        p.declaring_dir = dir;
        p.from = from;
        p.root_declared = from == "root";
        p.locked = lock_ ? lock_->find(spec.name) : nullptr;
        std::string identity = sources_.identity(spec, dir);
        // A dependency of a package version on a name its registry doesn't have rules that version out; the
        // project's own dependencies must exist (Sources::candidates says so).
        if (spec.kind == Kind::Registry && from != "root") {
            Registry& registry = sources_.registries().get(spec.registry);
            if (registry.package(spec.name) == nullptr) p.missing = registry.base();
        }
        if (p.missing.empty()) {
            std::vector<semver::Version> best_first = sources_.candidates(spec, dir, p.locked);
            p.versions = best_first;
            std::sort(p.versions.begin(), p.versions.end());
            p.versions.erase(std::unique(p.versions.begin(), p.versions.end()), p.versions.end());
            for (const auto& v : best_first) {
                auto at = static_cast<std::size_t>(std::lower_bound(p.versions.begin(), p.versions.end(), v) - p.versions.begin());
                if (std::find(p.preference.begin(), p.preference.end(), at) == p.preference.end()) p.preference.push_back(at);
            }
        }
        int id = static_cast<int>(packages_.size());
        packages_.push_back(std::move(p));
        identity_[id] = std::move(identity);
        acc_.emplace_back();
        by_name_.emplace(spec.name, id);
        return id;
    }

    // "not <dependency>": the versions of `pkg` that `spec` accepts. Git and path specs fix the source, not the
    // version, so they accept every version.
    Term requirement(int pkg, const manifest::DependencySpec& spec) const {
        const Package& p = packages_[pkg];
        Term t{pkg, false, Bits(p.versions.size()), {}};
        if (spec.kind == Kind::Registry) {
            auto range = semver::Range::parse(spec.range);
            for (std::size_t i = 0; i < p.versions.size(); ++i) {
                if (range && range->contains(p.versions[i])) t.set.set(i);
            }
            t.label = spec.range;
        } else {
            t.set = ~t.set;
            t.label = (p.versions.size() == 1 ? p.versions[0].to_string() + " " : "") + "(" + source_text(spec) + ")";
        }
        return t;
    }

    // The neighbouring versions of a registry package (in version order) that declare the same requirement as
    // version `i` does (§4.2), as a closed index range.
    std::pair<std::size_t, std::size_t> run_of(int pkg, std::size_t i, const manifest::DependencySpec& spec) {
        if (pkg == kProject || packages_[pkg].spec.kind != Kind::Registry) return {i, i};
        auto same = [&](std::size_t j) {
            for (const auto& d : meta(pkg, j).manifest.dependencies) {
                if (d.name == spec.name) {
                    return d.kind == spec.kind && d.range == spec.range && d.registry == spec.registry &&
                           d.git_url == spec.git_url && d.git_ref == spec.git_ref && d.path == spec.path;
                }
            }
            return false;
        };
        std::size_t lo = i, hi = i;
        while (lo > 0 && same(lo - 1)) --lo;
        while (hi + 1 < packages_[pkg].versions.size() && same(hi + 1)) ++hi;
        return {lo, hi};
    }

    // The incompatibilities for the dependencies of `pkg` at `version`, created the first time they are needed.
    std::vector<int> add_dependencies(int pkg, std::size_t version) {
        std::vector<int> ids;
        std::string from = pkg == kProject ? "root" : key(pkg, version);
        for (const auto& [spec, dir] : dependencies(pkg, version)) {
            int dep = package_for(spec, dir, from);
            auto [lo, hi] = run_of(pkg, version, spec);
            auto memo = std::make_tuple(pkg, dep, lo, hi);
            if (auto it = dependency_ids_.find(memo); it != dependency_ids_.end()) {
                ids.push_back(it->second);
                continue;
            }
            Term dependent{pkg, true, Bits(packages_[pkg].versions.size()), {}};
            for (std::size_t j = lo; j <= hi; ++j) dependent.set.set(j);
            Term needed = requirement(dep, spec);
            int id;
            if (needed.set.none()) {
                // Nothing can satisfy it, so these versions can't be used at all.
                id = create({dependent}, Cause::Dependency);
                incompatibilities_[id].unmet = negate(needed);
            } else {
                id = create({dependent, needed}, Cause::Dependency);
            }
            attach(id);
            dependency_ids_.emplace(memo, id);
            ids.push_back(id);
        }
        return ids;
    }

    // --- incompatibilities ------------------------------------------------------------------------

    int create(std::vector<Term> terms, Cause cause, int left = -1, int right = -1) {
        // The project is always selected, so it adds nothing to a derived incompatibility.
        if (cause == Cause::Derived && terms.size() > 1) {
            std::erase_if(terms, [](const Term& t) { return t.pkg == kProject && t.positive; });
        }
        // Terms about one package must hold together, so they intersect.
        std::vector<Term> merged;
        for (auto& t : terms) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const Term& m) { return m.pkg == t.pkg; });
            if (it == merged.end()) merged.push_back(std::move(t));
            else *it = intersect(*it, t);
        }
        incompatibilities_.push_back({std::move(merged), cause, left, right, std::nullopt});
        return static_cast<int>(incompatibilities_.size()) - 1;
    }

    void attach(int id) {
        for (const auto& t : incompatibilities_[id].terms) packages_[t.pkg].incompatibilities.push_back(id);
    }

    bool is_failure(const Incompatibility& inc) const {
        return inc.terms.empty() || (inc.terms.size() == 1 && inc.terms[0].pkg == kProject && inc.terms[0].positive);
    }

    // --- the partial solution ---------------------------------------------------------------------

    enum class Relation { Satisfied, Contradicted, Inconclusive };

    Relation relation(const Term& t) const {
        // With no assignment yet, the package may be anything, including not selected.
        if (!acc_[t.pkg]) return Relation::Inconclusive;
        if (satisfies(*acc_[t.pkg], t)) return Relation::Satisfied;
        if (is_empty(intersect(*acc_[t.pkg], t))) return Relation::Contradicted;
        return Relation::Inconclusive;
    }

    void assign(Term term, int cause) {
        int pkg = term.pkg;
        acc_[pkg] = acc_[pkg] ? intersect(*acc_[pkg], term) : term;
        assignments_.push_back({std::move(term), level_, cause});
    }

    void decide(int pkg, std::size_t version) {
        ++level_;
        packages_[pkg].decided = version;
        assign({pkg, true, Bits::only(packages_[pkg].versions.size(), version), {}}, -1);
    }

    void backtrack(int level) {
        while (!assignments_.empty() && assignments_.back().level > level) {
            if (assignments_.back().cause < 0) packages_[assignments_.back().term.pkg].decided.reset();
            assignments_.pop_back();
        }
        level_ = level;
        for (auto& a : acc_) a.reset();
        for (const auto& a : assignments_) acc_[a.term.pkg] = acc_[a.term.pkg] ? intersect(*acc_[a.term.pkg], a.term) : a.term;
    }

    // The earliest assignment that, with the ones before it, satisfies `term`.
    std::size_t satisfier(const Term& term) const {
        std::optional<Term> acc;
        for (std::size_t i = 0; i < assignments_.size(); ++i) {
            const Term& t = assignments_[i].term;
            if (t.pkg != term.pkg) continue;
            acc = acc ? intersect(*acc, t) : t;
            if (satisfies(*acc, term)) return i;
        }
        throw Error(std::format("internal error in dependency resolution: nothing satisfies a term on {}", name(term.pkg)));
    }

    // --- the loop ---------------------------------------------------------------------------------

    // What the incompatibility forces: nullopt when it is contradicted or says nothing yet; -1 when every term is
    // satisfied (a conflict); otherwise the package of the one term it derives a new assignment for.
    std::optional<int> propagate_one(int id) {
        const Incompatibility& inc = incompatibilities_[id];
        std::optional<std::size_t> open;
        for (std::size_t i = 0; i < inc.terms.size(); ++i) {
            Relation r = relation(inc.terms[i]);
            if (r == Relation::Contradicted) return std::nullopt;
            if (r == Relation::Inconclusive) {
                if (open) return std::nullopt;
                open = i;
            }
        }
        if (!open) return -1;
        Term derived = negate(inc.terms[*open]);
        int pkg = derived.pkg;
        assign(std::move(derived), id);
        return pkg;
    }

    void propagate(int start) {
        std::vector<int> changed = {start};
        while (!changed.empty()) {
            int pkg = changed.front();
            changed.erase(changed.begin());
            std::vector<int> ids = packages_[pkg].incompatibilities;
            // Newest first: learned incompatibilities are more general, so they derive more sooner.
            for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
                std::optional<int> r = propagate_one(*it);
                if (r && *r == -1) {
                    int cause = resolve_conflict(*it);
                    changed.clear();
                    if (auto next = propagate_one(cause); next && *next >= 0) changed.push_back(*next);
                    break;
                }
                if (r && std::find(changed.begin(), changed.end(), *r) == changed.end()) changed.push_back(*r);
            }
        }
    }

    // Decides the next package (§4.2), or returns nullopt when every required package has a version.
    std::optional<int> choose() {
        int best = -1;
        std::size_t best_count = 0;
        for (int pkg = 0; pkg < static_cast<int>(packages_.size()); ++pkg) {
            const auto& acc = acc_[pkg];
            if (packages_[pkg].decided || !acc || !acc->positive) continue;
            std::size_t n = acc->set.count();
            if (best < 0 || n < best_count || (n == best_count && packages_[pkg].name < packages_[best].name)) {
                best = pkg;
                best_count = n;
            }
        }
        if (best < 0) return std::nullopt;

        const Term allowed = *acc_[best];
        const auto& order = packages_[best].preference;
        auto pick = std::find_if(order.begin(), order.end(), [&](std::size_t i) { return allowed.set.test(i); });
        if (pick == order.end()) {
            attach(create({allowed}, Cause::NoVersions));
            return best;
        }
        // Deciding a version whose dependencies already conflict would only be undone: propagate instead.
        bool conflict = false;
        for (int id : add_dependencies(best, *pick)) {
            const auto& terms = incompatibilities_[id].terms;
            conflict = conflict || std::all_of(terms.begin(), terms.end(), [&](const Term& t) {
                           return t.pkg == best || relation(t) == Relation::Satisfied;
                       });
        }
        if (!conflict) decide(best, *pick);
        return best;
    }

    // Conflict resolution: from an incompatibility the partial solution satisfies, derive its root cause, backjump
    // to where that cause allows a new assignment, and return it (§4.2).
    int resolve_conflict(int id) {
        bool learned = false;
        while (!is_failure(incompatibilities_[id])) {
            const std::vector<Term> terms = incompatibilities_[id].terms;
            std::size_t recent_term = 0, recent = 0;
            bool first = true;
            std::optional<Term> difference;
            int previous_level = 1;  // the project's decision: stopping there gives clearer explanations
            for (std::size_t i = 0; i < terms.size(); ++i) {
                std::size_t s = satisfier(terms[i]);
                if (first) {
                    recent_term = i;
                    recent = s;
                    first = false;
                } else if (recent < s) {
                    previous_level = std::max(previous_level, assignments_[recent].level);
                    recent_term = i;
                    recent = s;
                    difference.reset();
                } else {
                    previous_level = std::max(previous_level, assignments_[s].level);
                }
                if (recent_term == i) {
                    // The satisfier may satisfy the term only together with earlier assignments.
                    Term rest = intersect(assignments_[recent].term, negate(terms[i]));
                    if (is_empty(rest)) {
                        difference.reset();
                    } else {
                        previous_level = std::max(previous_level, assignments_[satisfier(negate(rest))].level);
                        difference = std::move(rest);
                    }
                }
            }
            const Assignment satisfier_assignment = assignments_[recent];
            if (previous_level < satisfier_assignment.level || satisfier_assignment.cause < 0) {
                backtrack(previous_level);
                if (learned) attach(id);
                return id;
            }
            std::vector<Term> prior;
            for (std::size_t i = 0; i < terms.size(); ++i) {
                if (i != recent_term) prior.push_back(terms[i]);
            }
            for (const auto& t : incompatibilities_[satisfier_assignment.cause].terms) {
                if (t.pkg != satisfier_assignment.term.pkg) prior.push_back(t);
            }
            if (difference) prior.push_back(negate(*difference));
            id = create(std::move(prior), Cause::Derived, id, satisfier_assignment.cause);
            learned = true;
        }
        fail(id);
    }

    // --- explanations (§4.3) ----------------------------------------------------------------------

    std::string name(int pkg) const { return pkg == kProject ? "the project" : packages_[pkg].name; }

    // "1.2.0", ">=1.2.0", "<=1.4.0", ">=1.2.0 <=1.4.0", joined by " || "
    std::string versions_text(const Package& p, const Bits& set) const {
        std::vector<std::string> runs;
        std::size_t n = p.versions.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (!set.test(i)) continue;
            std::size_t j = i;
            while (j + 1 < n && set.test(j + 1)) ++j;
            const std::string lo = p.versions[i].to_string(), hi = p.versions[j].to_string();
            if (i == j) runs.push_back(lo);
            else if (i == 0) runs.push_back("<=" + hi);
            else if (j + 1 == n) runs.push_back(">=" + lo);
            else runs.push_back(std::format(">={} <={}", lo, hi));
            i = j;
        }
        std::string out;
        for (const auto& r : runs) out += (out.empty() ? "" : " || ") + r;
        return runs.empty() ? "(no version)" : out;
    }

    // A term without its polarity: "fmt ^10.2.0", "spdlog >=1.12.0", "every version of x" (or "x" when `bare`).
    std::string term_text(const Term& t, bool bare = false) const {
        if (t.pkg == kProject) return "the project";
        const Package& p = packages_[t.pkg];
        if (!t.label.empty()) return p.name + " " + t.label;
        std::string source = p.spec.kind == Kind::Registry ? "" : " (" + source_text(p.spec) + ")";
        if (p.versions.size() > 1 && t.set == ~Bits(p.versions.size())) return bare ? p.name : "every version of " + p.name;
        return p.name + " " + versions_text(p, t.set) + source;
    }

    std::string available(const Package& p) const {
        std::vector<std::string> list;
        for (std::size_t i = 0; i < p.preference.size() && i < 12; ++i) list.push_back(p.versions[p.preference[i]].to_string());
        if (list.empty()) return "";
        std::string out;
        for (const auto& v : list) out += (out.empty() ? "" : ", ") + v;
        return " (available: " + out + (p.preference.size() > list.size() ? ", …" : "") + ")";
    }

    std::string text(int id) const {
        const Incompatibility& inc = incompatibilities_[id];
        switch (inc.cause) {
            case Cause::Root: return "the project must be resolved";
            case Cause::Dependency: {
                std::string who = term_text(inc.terms[0]);
                if (!inc.unmet) return who + " depends on " + term_text(inc.terms[1]);
                const Package& dep = packages_[inc.unmet->pkg];
                if (!dep.missing.empty()) return std::format("{} depends on {}, which {} doesn't have", who, dep.name, dep.missing);
                if (dep.spec.kind != Kind::Registry && dep.versions.size() == 1) {
                    return std::format("{} depends on {}, which doesn't match {} {} ({})", who, term_text(*inc.unmet), dep.name,
                                       dep.versions[0].to_string(), source_text(dep.spec));
                }
                return std::format("{} depends on {}, which matches no version{}", who, term_text(*inc.unmet), available(dep));
            }
            case Cause::NoVersions:
                return std::format("{} matches no version{}", term_text(inc.terms[0]), available(packages_[inc.terms[0].pkg]));
            case Cause::Derived: break;
        }
        if (is_failure(inc)) return "no versions satisfy every requirement";
        std::vector<std::string> pos, neg;
        for (const auto& t : inc.terms) (t.positive ? pos : neg).push_back(term_text(t));
        auto bare = [&](bool positive) {
            for (const auto& t : inc.terms) {
                if (t.positive == positive) return term_text(t, true);
            }
            return std::string();
        };
        if (pos.size() == 1 && neg.empty()) return bare(true) + " can't be used";
        if (pos.empty() && neg.size() == 1) return bare(false) + " is required";
        if (pos.size() == 1) return pos[0] + " depends on " + join(neg, "or");
        if (neg.empty()) return join(pos, "and") + " can't be used together";
        if (pos.empty()) return "one of " + join(neg, "or") + " is required";
        return join(pos, "and") + " together depend on " + join(neg, "or");
    }

    class Explanation {
    public:
        explicit Explanation(const Solver& s) : s_(s) {}

        std::vector<std::string> lines(int failure) {
            visit(failure);
            if (!derived(failure)) {
                std::string only = s_.text(failure);
                only[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(only[0])));
                return {only + "."};
            }
            build(failure);
            std::vector<std::string> out;
            for (const auto& [text, number] : lines_) out.push_back(number ? std::format("({}) {}", *number, text) : text);
            for (auto it = out.rbegin(); it != out.rend(); ++it) {
                if (auto at = it->find("And because "); at != std::string::npos) {
                    it->replace(at, 12, "So, because ");
                    break;
                }
                if (!it->empty()) break;
            }
            return out;
        }

    private:
        bool derived(int id) const { return s_.incompatibilities_[id].cause == Cause::Derived; }
        const Incompatibility& inc(int id) const { return s_.incompatibilities_[id]; }

        // Derived incompatibilities reached more than once get a line number (§4.3).
        void visit(int id) {
            if (!derived(id)) return;
            if (!seen_.insert(id).second) {
                shared_.insert(id);
                return;
            }
            visit(inc(id).left);
            visit(inc(id).right);
        }

        void line(std::string text) { lines_.emplace_back(std::move(text), std::nullopt); }

        void number(int id) {
            lines_.back().second = ++count_;
            numbers_[id] = count_;
        }

        std::optional<int> ref(int id) const {
            auto it = numbers_.find(id);
            return it == numbers_.end() ? std::nullopt : std::optional<int>(it->second);
        }

        std::string t(int id) const { return s_.text(id); }

        // "A and B", with a comma before "and" when A ends in a clause of its own ("…, which matches no version").
        static std::string both(const std::string& a, const std::string& b) {
            return a + (a.find(", which ") != std::string::npos ? ", and " : " and ") + b;
        }

        void build(int id) {
            helper(id);
            if (shared_.contains(id) && !ref(id)) number(id);
        }

        void helper(int id) {
            int a = inc(id).left, b = inc(id).right;
            bool da = derived(a), db = derived(b);
            if (!da && !db) {
                // A chain reads in order: "foo depends on a" before "a depends on b".
                const Incompatibility &ia = inc(a), &ib = inc(b);
                if (ia.cause == Cause::Dependency && ib.cause == Cause::Dependency && !ib.unmet && ib.terms[1].pkg == ia.terms[0].pkg) {
                    std::swap(a, b);
                }
                line(std::format("Because {}, {}.", both(t(a), t(b)), t(id)));
            } else if (da != db) {
                one_each(da ? a : b, da ? b : a, id);
            } else if (auto ra = ref(a), rb = ref(b); ra && rb) {
                line(std::format("Because {}, {}.", both(std::format("{} ({})", t(a), *ra), std::format("{} ({})", t(b), *rb)), t(id)));
            } else if (ra || rb) {
                int known = ra ? a : b;
                build(ra ? b : a);
                line(std::format("And because {} ({}), {}.", t(known), *ref(known), t(id)));
            } else {
                build(a);
                if (shared_.contains(a)) {
                    line("");
                    helper(id);
                } else {
                    number(a);
                    line("");
                    build(b);
                    line(std::format("And because {} ({}), {}.", t(a), *ref(a), t(id)));
                }
            }
        }

        void one_each(int derived_id, int external, int id) {
            if (auto r = ref(derived_id)) {
                line(std::format("Because {}, {}.", both(std::format("{} ({})", t(derived_id), *r), t(external)), t(id)));
                return;
            }
            int a = inc(derived_id).left, b = inc(derived_id).right;
            if (derived(a) != derived(b)) {
                build(derived(a) ? a : b);
                line(std::format("And because {}, {}.", both(t(derived(a) ? b : a), t(external)), t(id)));
            } else {
                build(derived_id);
                line(std::format("And because {}, {}.", t(external), t(id)));
            }
        }

        const Solver& s_;
        std::set<int> seen_, shared_;
        std::map<int, int> numbers_;
        int count_ = 0;
        std::vector<std::pair<std::string, std::optional<int>>> lines_;
    };

    // The project's dependencies named in the explanation, which are what the project can change.
    std::string hint(int failure) const {
        std::set<std::string> project, others;
        std::set<int> seen;
        std::function<void(int)> walk = [&](int id) {
            if (!seen.insert(id).second) return;
            const Incompatibility& inc = incompatibilities_[id];
            if (inc.cause == Cause::Derived) {
                walk(inc.left);
                walk(inc.right);
            } else if (inc.cause == Cause::Dependency) {
                const Term& dep = inc.unmet ? *inc.unmet : inc.terms[1];
                if (inc.terms[0].pkg == kProject) project.insert(packages_[dep.pkg].name);
                else others.insert(packages_[inc.terms[0].pkg].name);
                // A git or path source the project chose is the project's to change too.
                if (inc.unmet && packages_[dep.pkg].root_declared) project.insert(packages_[dep.pkg].name);
            }
        };
        walk(failure);
        std::string out;
        if (!project.empty()) {
            out = std::format("change the range of {} in cpm.jsonc", join(std::vector<std::string>(project.begin(), project.end()), "or"));
        }
        if (!others.empty()) out += (out.empty() ? "" : "; ") + std::string("cpm info <package> shows what each version depends on");
        return out;
    }

    [[noreturn]] void fail(int id) const {
        std::string message = "the project's dependencies can't be resolved";
        for (const auto& l : Explanation(*this).lines(id)) message += l.empty() ? "\n" : "\n  " + l;
        throw Error(message, hint(id));
    }

    const manifest::Manifest& root_;
    Sources& sources_;
    const lock::Lockfile* lock_;
    std::vector<Package> packages_;
    std::map<std::string, int> by_name_;
    std::map<int, std::string> identity_;
    std::vector<Incompatibility> incompatibilities_;
    std::map<std::tuple<int, int, std::size_t, std::size_t>, int> dependency_ids_;
    std::vector<Assignment> assignments_;
    std::vector<std::optional<Term>> acc_;  // per package: the intersection of its assignments
    int level_ = 0;
};

Resolution Solver::result() const {
    Resolution res;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto* list : {&root_.dependencies, &root_.dev_dependencies}) {
        for (const auto& d : *list) dependents[d.name].push_back("root");
    }
    std::map<std::string, const PackageMeta*> chosen;
    for (int pkg = 1; pkg < static_cast<int>(packages_.size()); ++pkg) {
        const Package& p = packages_[pkg];
        if (!p.decided) continue;
        const PackageMeta& m = sources_.describe(p.spec, p.declaring_dir, p.versions[*p.decided], p.locked);
        chosen.emplace(p.name, &m);
        for (const auto& d : m.manifest.dependencies) dependents[d.name].push_back(p.name + "@" + m.version.to_string());
    }
    for (const auto& [name, meta] : chosen) {
        ResolvedPackage p;
        p.meta = *meta;
        p.dependents = dependents[name];
        res.packages.emplace(name, std::move(p));
    }

    // dev: not reachable from the root's regular dependencies
    std::set<std::string> regular;
    std::function<void(const std::string&)> mark = [&](const std::string& name) {
        if (!res.packages.contains(name) || !regular.insert(name).second) return;
        for (const auto& d : res.packages.at(name).meta.manifest.dependencies) mark(d.name);
    };
    for (const auto& d : root_.dependencies) mark(d.name);
    for (auto& [name, p] : res.packages) p.dev = !regular.contains(name);

    // Dependency order (Kahn): a package comes out once everything that depends on it has.
    std::map<std::string, int> count;
    for (const auto& [name, p] : res.packages) count.try_emplace(name, 0);
    for (const auto& [name, p] : res.packages) {
        for (const auto& d : p.meta.manifest.dependencies) ++count[d.name];
    }
    std::set<std::string> ready;
    for (const auto& [name, n] : count) {
        if (n == 0) ready.insert(name);
    }
    while (!ready.empty()) {
        std::string name = *ready.begin();
        ready.erase(ready.begin());
        res.order.push_back(name);
        for (const auto& d : res.packages.at(name).meta.manifest.dependencies) {
            if (--count[d.name] == 0) ready.insert(d.name);
        }
    }
    if (res.order.size() != res.packages.size()) {
        std::vector<std::string> cycle;
        for (const auto& [name, n] : count) {
            if (n > 0) cycle.push_back(name);
        }
        std::string list;
        for (const auto& c : cycle) list += (list.empty() ? "" : ", ") + c;
        throw Error(std::format("dependency cycle between: {}", list));
    }
    return res;
}

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
    Solver solver(root, sources, lock);
    solver.solve();
    Resolution res = solver.result();
    check_std(root, res);
    return res;
}

}  // namespace cpm::install
