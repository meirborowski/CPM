#include <format>

#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "publish/publish.hpp"

namespace cpm::cli {

int cmd_publish(const ParsedArgs& args, Context& ctx) {
    if (!args.positionals.empty()) throw UsageError("cpm publish takes no arguments; it publishes the current package");
    if (args.flag("with-binary")) {
        throw UsageError("--with-binary is not supported yet",
                         "publish the source package; dependents build it once per ABI in their store");
    }
    auto root = paths::find_project_root(ctx.cwd);
    if (!root) {
        throw Error(std::format("no cpm.jsonc found in {} or any parent directory", paths::to_utf8(ctx.cwd)),
                    "run cpm publish inside the package to publish");
    }
    publish::Options options;
    options.registry = args.value("registry").value_or("");
    if (auto out = args.value("out")) options.out = ctx.cwd / paths::from_utf8(*out);
    options.dry_run = args.flag("dry-run");
    if (options.dry_run && options.out) throw UsageError("--dry-run and --out exclude each other");
    publish::publish(*root, options, load_config(), paths::cpm_home());
    return 0;
}

}  // namespace cpm::cli
