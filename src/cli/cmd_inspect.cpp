// cpm info / tree / why: looking at a package on a registry, and at the project's resolved dependencies
// (docs/spec/cli.md).
#include <charconv>
#include <format>
#include <map>

#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "install/inspect.hpp"
#include "install/installer.hpp"
#include "install/outdated.hpp"
#include "install/registry.hpp"
#include "install/service.hpp"
#include "install/store.hpp"
#include "lock/lockfile.hpp"
#include "semver/range.hpp"

namespace cpm::cli {

namespace {

namespace fs = std::filesystem;

// The project's manifest and lockfile, for commands that show what was resolved.
struct Resolved {
    fs::path root;
    manifest::Manifest manifest;
    lock::Lockfile lock;

    std::string label() const { return manifest.name + " " + manifest.version.to_string(); }
};

Resolved load_resolved(const Context& ctx, std::string_view command) {
    auto root = paths::find_project_root(ctx.cwd);
    if (!root) {
        throw Error(std::format("no cpm.jsonc found in {} or any parent directory", paths::to_utf8(ctx.cwd)),
                    std::format("run cpm {} inside a package", command));
    }
    manifest::Manifest m = manifest::load(*root / "cpm.jsonc", ctx.cwd);
    for (const auto& w : m.warnings) log::diagnostic(w);
    std::optional<lock::Lockfile> lock = lock::load(*root, ctx.cwd);
    if (!lock) throw Error("there is no cpm-lock.json yet, so no dependencies have been resolved", "run cpm install");
    try {
        auto why = lock::staleness(*lock, m, [&](const lock::Package& p) { return install::path_package_manifest(*root, m, p); });
        if (why) log::info(std::format("note: cpm-lock.json is out of date ({}); run cpm install to update it", *why));
    } catch (const Error& e) {
        log::info(std::format("note: cpm-lock.json may be out of date: {}", e.what()));
    }
    return {*root, std::move(m), std::move(*lock)};
}

// An argument the shell would split or redirect, quoted for the "install with" line.
std::string shell_word(const std::string& word) {
    bool plain = word.find_first_of(" <>|&;*?\"'$`()") == std::string::npos;
    return plain ? word : "\"" + word + "\"";
}

}  // namespace

int cmd_info(const ParsedArgs& args, Context&) {
    if (args.positionals.size() != 1) {
        throw UsageError("cpm info takes one package: <name> or <name>@<range>", "e.g. cpm info fmt, or cpm info fmt@^11");
    }
    const std::string& arg = args.positionals[0];
    std::size_t at = arg.find('@');
    std::string name = arg.substr(0, at);
    if (!manifest::valid_package_name(name)) throw UsageError(std::format("invalid package name '{}'", name));
    std::optional<semver::Range> range;
    if (at != std::string::npos) {
        std::string text = arg.substr(at + 1);
        range = semver::Range::parse(text);
        if (text.empty() || !range) {
            throw UsageError(std::format("invalid version range '{}'", text), "use a range such as ^1.2.0, ~1.4 or >=2 <3");
        }
    }

    fs::path home = paths::cpm_home();
    std::string registry_arg = args.value("registry").value_or("");
    install::Registries registries(load_config(), home, install::Freshness::Online);
    install::Registry& registry = registries.get(registry_arg);
    const install::RegistryPackage* package = registry.package(name);
    if (package == nullptr) {
        throw Error(std::format("the registry {} has no package \"{}\"", registry.base(), name),
                    registry.api() ? std::format("cpm search {} finds packages with similar names", name) : "check the name");
    }
    auto version = install::newest(*package, range ? &*range : nullptr);
    if (!version && range) {
        throw Error(std::format("no version of {} on {} satisfies {}", name, registry.base(), range->text()),
                    std::format("cpm info {} lists its versions", name));
    }
    if (!version) throw Error(std::format("every version of {} on {} is yanked", name, registry.base()));

    std::optional<install::service::PackageDetails> details;
    try {
        details = install::service::details(registry, name, home);
    } catch (const Error& e) {
        log::warn(std::format("could not read more about {} from {}: {}", name, registry.base(), e.what()),
                  "showing what its index document says");
    }
    std::string command = "cpm install " + shell_word(arg) + (registry_arg.empty() ? "" : " --registry " + shell_word(registry_arg));
    log::out(install::render_info(*package, *package->find(*version), details, command));
    return 0;
}

int cmd_tree(const ParsedArgs& args, Context& ctx) {
    if (!args.positionals.empty()) throw UsageError("cpm tree takes no arguments", "cpm why <name> shows what requires a package");
    install::TreeOptions options{.depth = std::nullopt, .prod = args.flag("prod")};
    if (auto depth = args.value("depth")) {
        int n = -1;
        auto [end, ec] = std::from_chars(depth->data(), depth->data() + depth->size(), n);
        if (ec != std::errc() || end != depth->data() + depth->size() || n < 0) {
            throw UsageError(std::format("--depth needs a number of levels, not '{}'", *depth), "e.g. --depth 1 for direct dependencies only");
        }
        options.depth = n;
    }
    Resolved r = load_resolved(ctx, "tree");
    log::out(install::render_tree(r.lock, r.label(), options));
    return 0;
}

int cmd_why(const ParsedArgs& args, Context& ctx) {
    if (args.positionals.size() != 1) throw UsageError("cpm why takes one package name", "e.g. cpm why fmt");
    Resolved r = load_resolved(ctx, "why");
    fs::path home = paths::cpm_home();

    // Each dependent's manifest, read once: the project's, a path package's, or a store entry's when it is there.
    std::map<std::string, std::optional<manifest::Manifest>> manifests;
    auto manifest_of = [&](const lock::Package& p) -> const std::optional<manifest::Manifest>& {
        auto [it, inserted] = manifests.try_emplace(p.key());
        if (!inserted) return it->second;
        try {
            if (std::holds_alternative<lock::PathResolution>(p.resolution)) {
                it->second = install::path_package_manifest(r.root, r.manifest, p);
            } else if (fs::path entry = install::entry_dir(home, p); install::entry_intact(entry)) {
                it->second = install::entry_manifest(entry, p);
            }
        } catch (const Error&) {
            it->second.reset();  // unreadable: show the locked version instead
        }
        return it->second;
    };
    auto requirement_of = [&](const lock::Package* dependent, const std::string& name) -> std::optional<std::string> {
        const manifest::Manifest* m = &r.manifest;
        if (dependent != nullptr) {
            const auto& loaded = manifest_of(*dependent);
            if (!loaded) return std::nullopt;
            m = &*loaded;
        }
        for (const auto& spec : m->dependencies) {
            if (spec.name == name) return install::requirement(spec);
        }
        if (dependent == nullptr) {
            for (const auto& spec : m->dev_dependencies) {
                if (spec.name == name) return install::requirement(spec);
            }
        }
        return std::nullopt;
    };
    log::out(install::render_why(r.lock, r.label(), args.positionals[0], requirement_of));
    return 0;
}

}  // namespace cpm::cli
