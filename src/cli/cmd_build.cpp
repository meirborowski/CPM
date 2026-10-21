#include <format>

#include "build/pipeline.hpp"
#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"

namespace cpm::cli {

int cmd_build(const ParsedArgs& args, Context& ctx) {
    if (!args.positionals.empty()) throw UsageError(std::format("unexpected argument '{}'", args.positionals.front()));
    manifest::UserConfig config = load_config();

    build::BuildRequest req;
    req.cwd = ctx.cwd;
    req.profile = profile_option(args);
    req.toolchain = toolchain_option(args);
    req.jobs = jobs_option(args, config);
    req.clean = args.flag("clean");
    req.targets = args.flag("tests") ? build::BuildRequest::Targets::MainAndTests : build::BuildRequest::Targets::Main;
    req.dependencies = dependency_provider(config);
    return build::run_build(req, config).exit_code;
}

}  // namespace cpm::cli
