#include <algorithm>
#include <array>
#include <format>

#include "build/pipeline.hpp"
#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "install/installer.hpp"
#include "install/outdated.hpp"
#include "lock/lockfile.hpp"

namespace cpm::cli {

namespace {

int run_install(const ParsedArgs& args, Context& ctx, install::Options options) {
    manifest::UserConfig config = load_config();
    build::Project project = build::load_project(ctx.cwd, profile_option(args), toolchain_option(args), config);
    options.cwd = ctx.cwd;
    install::install(project, options, config);
    return 0;
}

std::string version_or_dash(const std::optional<semver::Version>& v) { return v ? v->to_string() : "-"; }

// Columns separated by two spaces, each as wide as its widest cell.
void print_table(const std::vector<std::array<std::string, 4>>& rows) {
    std::array<std::size_t, 4> width{};
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) width[i] = std::max(width[i], row[i].size());
    }
    for (const auto& row : rows) {
        log::out(std::format("{:<{}}  {:<{}}  {:<{}}  {}\n", row[0], width[0], row[1], width[1], row[2], width[2], row[3]));
    }
}

}  // namespace

int cmd_install(const ParsedArgs& args, Context& ctx) {
    install::Options options;
    options.add = args.positionals;
    options.dev = args.flag("save-dev");
    options.exact = args.flag("save-exact");
    options.frozen = args.flag("frozen-lockfile");
    options.offline = args.flag("offline");
    options.prefer_offline = args.flag("prefer-offline");
    options.no_build = args.flag("no-build");
    options.registry = args.value("registry").value_or("");
    if (options.offline && options.prefer_offline) throw UsageError("--offline and --prefer-offline exclude each other");
    if (!options.registry.empty() && options.add.empty())
        throw UsageError("--registry applies to the packages being added", "e.g. cpm install fmt --registry internal");
    return run_install(args, ctx, std::move(options));
}

int cmd_update(const ParsedArgs& args, Context& ctx) {
    install::Options options;
    options.update = args.positionals;
    options.update_all = args.positionals.empty();
    options.no_build = args.flag("no-build");
    return run_install(args, ctx, std::move(options));
}

int cmd_remove(const ParsedArgs& args, Context& ctx) {
    if (args.positionals.empty()) throw UsageError("cpm remove needs the names of the packages to remove");
    install::Options options;
    options.remove = args.positionals;
    options.offline = args.flag("offline");
    return run_install(args, ctx, std::move(options));
}

int cmd_outdated(const ParsedArgs& args, Context& ctx) {
    if (!args.positionals.empty()) throw UsageError("cpm outdated takes no arguments; it checks every direct dependency");
    auto root = paths::find_project_root(ctx.cwd);
    if (!root) {
        throw Error(std::format("no cpm.jsonc found in {} or any parent directory", paths::to_utf8(ctx.cwd)),
                    "run cpm outdated inside a package");
    }
    manifest::Manifest m = manifest::load(*root / "cpm.jsonc", ctx.cwd);
    for (const auto& w : m.warnings) log::diagnostic(w);
    std::optional<lock::Lockfile> lock = lock::load(*root, ctx.cwd);
    install::Registries registries(load_config(), paths::cpm_home(), install::Freshness::Online);
    install::OutdatedReport report = install::outdated(m, lock ? &*lock : nullptr, registries);

    for (const auto* s : report.skipped) {
        std::string_view kind = s->kind == manifest::DependencySpec::Kind::Git ? "git" : "path";
        log::info(std::format("note: skipped {}, a {} dependency: cpm outdated checks registry dependencies only", s->name, kind));
    }
    if (!lock && !report.entries.empty()) log::info("note: there is no cpm-lock.json yet, so nothing is current; run cpm install");

    std::vector<std::array<std::string, 4>> rows = {{"Package", "Current", "Wanted", "Latest"}};
    for (const auto& e : report.entries) {
        if (e.outdated()) rows.push_back({e.name, version_or_dash(e.current), version_or_dash(e.wanted), version_or_dash(e.latest)});
    }
    if (rows.size() > 1) print_table(rows);
    else if (report.entries.empty()) log::info("No registry dependencies to check");
    else if (report.entries.size() == 1) log::info("The registry dependency is up to date");
    else log::info(std::format("All {} registry dependencies are up to date", report.entries.size()));
    if (lock) {
        for (const auto& note : install::unneeded_overrides(m, *lock)) log::info("note: " + note);
    }
    return 0;
}

}  // namespace cpm::cli
