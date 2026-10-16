#include <charconv>
#include <format>

#include "build/pipeline.hpp"
#include "build/tests.hpp"
#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/log.hpp"

namespace cpm::cli {

int cmd_test(const ParsedArgs& args, Context& ctx) {
    manifest::UserConfig config = load_config();

    build::BuildRequest req;
    req.cwd = ctx.cwd;
    req.profile = profile_option(args);
    req.cxx = args.value("cxx");
    req.jobs = jobs_option(args, config);
    req.targets = build::BuildRequest::Targets::Tests;
    req.test_filters = args.positionals;
    req.dependencies = dependency_provider(config);

    build::TestOptions opts;
    if (auto t = args.value("timeout")) {
        int secs = 0;
        auto [p, ec] = std::from_chars(t->data(), t->data() + t->size(), secs);
        if (ec != std::errc() || p != t->data() + t->size() || secs <= 0)
            throw UsageError(std::format("invalid --timeout '{}'", *t), "give a number of seconds, e.g. --timeout 60");
        opts.timeout = std::chrono::seconds(secs);
    }
    opts.show_output = log::verbose();

    build::BuildResult result = build::run_build(req, config);
    if (result.exit_code != 0) return result.exit_code;
    if (args.flag("no-run")) return 0;
    return build::run_tests(result, opts);
}

}  // namespace cpm::cli
