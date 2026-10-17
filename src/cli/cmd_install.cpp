#include <format>

#include "build/pipeline.hpp"
#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "install/installer.hpp"

namespace cpm::cli {

namespace {

int run_install(const ParsedArgs& args, Context& ctx, install::Options options) {
    manifest::UserConfig config = load_config();
    build::Project project = build::load_project(ctx.cwd, profile_option(args), args.value("cxx"), config);
    options.cwd = ctx.cwd;
    install::install(project, options, config);
    return 0;
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

}  // namespace cpm::cli
