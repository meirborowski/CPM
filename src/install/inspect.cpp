#include "install/inspect.hpp"

#include <format>
#include <set>
#include <variant>
#include <vector>

#include "core/error.hpp"
#include "core/strings.hpp"
#include "install/outdated.hpp"

namespace cpm::install {

namespace {

constexpr std::size_t kLabelWidth = 14;  // "dependencies" and two spaces
constexpr std::size_t kLineWidth = 80;
constexpr std::size_t kMaxVersions = 20;

std::string type_text(manifest::TargetKind kind) {
    switch (kind) {
        case manifest::TargetKind::Executable: return "executable";
        case manifest::TargetKind::StaticLib: return "static library";
        case manifest::TargetKind::SharedLib: return "shared library";
        case manifest::TargetKind::HeaderOnly: return "header-only library";
    }
    return "library";
}

// "c++17" -> "C++17"
std::string std_text(const std::string& std) { return std.starts_with("c++") ? "C++" + std.substr(3) : std; }

// 1234567 -> "1,234,567"
std::string grouped(std::int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return n < 0 ? "-" + out : out;
}

// "label         first, second, …", wrapped at kLineWidth with the items aligned under the first.
std::string field(std::string_view label, const std::vector<std::string>& items) {
    std::string out = std::format("{:<{}}", label, kLabelWidth);
    std::size_t column = kLabelWidth;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::string item = items[i] + (i + 1 < items.size() ? "," : "");
        std::size_t width = str::utf8_length(item);
        if (column > kLabelWidth && column + 1 + width > kLineWidth) {
            out += "\n" + std::string(kLabelWidth, ' ');
            column = kLabelWidth;
        } else if (column > kLabelWidth) {
            out += ' ';
            ++column;
        }
        out += item;
        column += width;
    }
    return out + "\n";
}

// --- trees -----------------------------------------------------------------------------------------

struct Node {
    std::string label;
    std::vector<Node> children;
};

void draw(const std::vector<Node>& children, const std::string& prefix, std::string& out) {
    for (std::size_t i = 0; i < children.size(); ++i) {
        bool last = i + 1 == children.size();
        out += prefix + (last ? "└── " : "├── ") + children[i].label + "\n";
        draw(children[i].children, prefix + (last ? "    " : "│   "), out);
    }
}

std::string render(const Node& root) {
    std::string out = root.label + "\n";
    draw(root.children, "", out);
    return out;
}

// "fmt 12.2.0", with the source of a git or path package.
std::string label(const lock::Package& p) {
    std::string out = p.name + " " + p.version.to_string();
    if (const auto* git = std::get_if<lock::GitResolution>(&p.resolution)) {
        out += " (git " + str::printable(git->url) + (git->ref.empty() ? "" : "#" + str::printable(git->ref)) + ")";
    } else if (const auto* path = std::get_if<lock::PathResolution>(&p.resolution)) {
        out += " (path " + str::printable(path->path) + ")";
    }
    return out;
}

const lock::Package* locked(const lock::Lockfile& lock, const std::string& name, const semver::Version& version) {
    auto it = lock.packages.find(name + "@" + version.to_string());
    return it == lock.packages.end() ? nullptr : &it->second;
}

class TreeBuilder {
public:
    TreeBuilder(const lock::Lockfile& lock, const TreeOptions& options) : lock_(lock), options_(options) {}

    Node node(const std::string& name, const semver::Version& version, int level, std::string_view note) {
        const lock::Package* p = locked(lock_, name, version);
        if (p == nullptr) return {name + " " + version.to_string() + " (missing from cpm-lock.json)", {}};
        Node n{label(*p) + std::string(note), {}};
        if (expanded_.contains(p->key())) {
            n.label += " (*)";
            return n;
        }
        if (options_.depth && level >= *options_.depth) return n;
        expanded_.insert(p->key());
        for (const auto& [dep, v] : p->dependencies) n.children.push_back(node(dep, v, level + 1, ""));
        return n;
    }

private:
    const lock::Lockfile& lock_;
    const TreeOptions& options_;
    std::set<std::string> expanded_;
};

class WhyBuilder {
public:
    WhyBuilder(const lock::Lockfile& lock, std::string_view project, const RequirementOf& requirement_of) :
        lock_(lock), project_(project), requirement_of_(requirement_of) {}

    // The steps from `p` to the packages that require it, and on up from each.
    std::vector<Node> dependents(const lock::Package& p) {
        std::vector<Node> steps;
        auto asks = [&](const lock::Package* dependent) { return requirement_of_(dependent, p.name).value_or(p.version.to_string()); };
        if (lock_.dependencies.contains(p.name)) steps.push_back({std::format("{} from {} (the project)", asks(nullptr), project_), {}});
        if (lock_.dev_dependencies.contains(p.name)) {
            steps.push_back({std::format("{} from {} (the project, dev)", asks(nullptr), project_), {}});
        }
        for (const auto& [key, q] : lock_.packages) {
            if (!q.dependencies.contains(p.name)) continue;
            Node step{std::format("{} from {}", asks(&q), label(q)), {}};
            if (!expanded_.insert(q.key()).second) {
                step.label += " (*)";
            } else {
                step.children = dependents(q);
            }
            steps.push_back(std::move(step));
        }
        return steps;
    }

    void expanded(const std::string& key) { expanded_.insert(key); }

private:
    const lock::Lockfile& lock_;
    std::string_view project_;
    const RequirementOf& requirement_of_;
    std::set<std::string> expanded_;
};

}  // namespace

std::string render_info(const RegistryPackage& package, const RegistryVersion& shown,
                        const std::optional<service::PackageDetails>& details, std::string_view install) {
    std::vector<std::string> head = {package.name + " " + shown.version.to_string(), type_text(shown.type)};
    if (!shown.std.empty()) head.push_back(std_text(str::printable(shown.std)));
    if (details && !str::printable(details->license).empty()) head.push_back(str::printable(details->license));
    std::string out = str::join(head, " · ") + "\n";
    if (details && !str::printable(details->description).empty()) out += str::printable(details->description) + "\n";
    out += "\n";

    if (details) {
        std::string repository = str::printable(details->repository), homepage = str::printable(details->homepage);
        if (!repository.empty()) out += field("repository", {repository});
        if (!homepage.empty() && homepage != repository) out += field("homepage", {homepage});
        std::vector<std::string> owners;
        for (const auto& o : details->owners) owners.push_back(str::printable(o));
        if (!owners.empty()) out += field("owners", owners);
        if (details->downloads) out += field("downloads", {grouped(*details->downloads)});
    }

    std::vector<std::string> dependencies;
    for (const auto& [name, range] : shown.dependencies) dependencies.push_back(str::printable(name) + " " + str::printable(range));
    out += field("dependencies", dependencies.empty() ? std::vector<std::string>{"none"} : dependencies);

    auto latest = newest(package, nullptr);
    std::vector<std::string> versions;
    for (const auto& v : package.versions) {
        if (versions.size() == kMaxVersions) break;
        std::string item = v.version.to_string();
        if (latest && v.version == *latest) item += " (latest)";
        if (v.yanked) item += " (yanked)";
        versions.push_back(std::move(item));
    }
    if (package.versions.size() > kMaxVersions) versions.push_back(std::format("and {} more", package.versions.size() - kMaxVersions));
    out += field("versions", versions);

    out += "\n" + field("install with", {std::string(install)});
    return out;
}

std::string render_tree(const lock::Lockfile& lock, std::string_view project, const TreeOptions& options) {
    TreeBuilder builder(lock, options);
    Node root{std::string(project), {}};
    if (!options.depth || *options.depth > 0) {
        for (const auto& [name, dep] : lock.dependencies) root.children.push_back(builder.node(name, dep.version, 1, ""));
        if (!options.prod) {
            for (const auto& [name, dep] : lock.dev_dependencies) root.children.push_back(builder.node(name, dep.version, 1, " (dev)"));
        }
    }
    return render(root);
}

std::string render_why(const lock::Lockfile& lock, std::string_view project, const std::string& name,
                       const RequirementOf& requirement_of) {
    const lock::Package* target = lock.find(name);
    if (target == nullptr) {
        throw Error(std::format("{} is not in cpm-lock.json, so nothing in this project depends on it", name),
                    "cpm tree lists the project's dependencies");
    }
    WhyBuilder builder(lock, project, requirement_of);
    builder.expanded(target->key());
    return render({label(*target), builder.dependents(*target)});
}

std::optional<std::string> overridden_requirement(const lock::Lockfile& lock, const lock::Package& dependent, const std::string& name) {
    auto own = dependent.overridden.find(name);
    const auto* applied = manifest::find_override(lock.overrides, dependent.name, name);
    if (own == dependent.overridden.end() || applied == nullptr) return std::nullopt;
    return std::format("{} (override of {})", applied->second, own->second);
}

std::string requirement(const manifest::DependencySpec& spec) {
    switch (spec.kind) {
        case manifest::DependencySpec::Kind::Registry: return spec.range;
        case manifest::DependencySpec::Kind::Git: return "git " + spec.git_url + (spec.git_ref.empty() ? "" : "#" + spec.git_ref);
        case manifest::DependencySpec::Kind::Path: return "path " + spec.path;
    }
    return spec.specifier;
}

}  // namespace cpm::install
