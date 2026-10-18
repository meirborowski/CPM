#include "cli/commands.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "setup/setup.hpp"

namespace cpm::cli {

int cmd_setup(const ParsedArgs& args, Context&) {
    if (!args.positionals.empty()) throw UsageError("cpm setup takes no arguments");
    setup::Options options;
    options.check = args.flag("check");
    options.yes = args.flag("yes");
    options.managed = args.flag("managed");
    if (options.check && options.managed) throw UsageError("--check and --managed exclude each other");
    return setup::run(options, paths::cpm_home());
}

}  // namespace cpm::cli
