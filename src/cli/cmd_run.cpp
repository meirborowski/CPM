#include <format>

#include "build/pipeline.hpp"
#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "manifest/manifest.hpp"
#include "run/run.hpp"

namespace cpm::cli {

namespace fs = std::filesystem;

namespace {

run::Environment environment(const fs::path& root, const std::string& profile, const std::string& name,
                             const std::string& version) {
    run::Environment env;
    env.root = root;
    env.profile = profile;
    env.package_name = name;
    env.package_version = version;
    env.bin_dir = root / "build" / paths::from_utf8(profile);
    env.cpm_exe = paths::current_exe();
    return env;
}

int run_script(const std::string& name, const ParsedArgs& args, Context& ctx, const std::string& profile) {
    auto root = paths::find_project_root(ctx.cwd);
    if (!root) {
        throw Error(std::format("no cpm.jsonc found in {} or any parent directory", paths::to_utf8(ctx.cwd)),
                    "run 'cpm init' to create a package");
    }
    manifest::Manifest m = manifest::load(*root / "cpm.jsonc", ctx.cwd);
    for (const auto& w : m.warnings) log::diagnostic(w);
    if (!m.profiles.contains(profile)) throw Error(std::format("unknown profile \"{}\"", profile));

    const std::string* script = nullptr;
    std::vector<std::string> names;
    for (const auto& [n, command] : m.scripts) {
        names.push_back(n);
        if (n == name) script = &command;
    }
    if (script == nullptr) {
        throw Error(std::format("no script named \"{}\" in cpm.jsonc", name),
                    names.empty() ? std::string("this package defines no \"scripts\"; run 'cpm run' without a name "
                                                "to build and run the executable")
                                  : std::format("defined scripts: {}", str::join(names, ", ")));
    }

    log::status("Running", std::format("{}: {}", name, *script));
    proc::RunOptions opts;
    opts.forward_interrupts = true;
    return proc::run(
        run::script_command(*script, args.passthrough, environment(*root, profile, m.name, m.version.to_string())),
        opts);
}

int run_program(const ParsedArgs& args, Context& ctx, const std::string& profile) {
    manifest::UserConfig config = load_config();
    build::BuildRequest req;
    req.cwd = ctx.cwd;
    req.profile = profile;
    req.cxx = args.value("cxx");
    req.jobs = jobs_option(args, config);
    req.require_executable = true;
    req.dependencies = dependency_provider(config);
    build::BuildResult built = build::run_build(req, config);
    if (built.exit_code != 0) return built.exit_code;

    const build::BuildPlan& plan = built.plan;
    fs::path exe = plan.root / paths::from_utf8(plan.main_output);

    std::string shown = paths::display(exe, ctx.cwd);
    for (const auto& a : args.passthrough) shown += ' ' + proc::quote(a);
    log::status("Running", shown);
    proc::RunOptions opts;
    opts.forward_interrupts = true;
    // The program runs where the user is, like any command they would type themselves.
    run::Environment env = environment(plan.root, profile, plan.package, plan.package_version);
    return proc::run(run::program_command(exe, args.passthrough, ctx.cwd, env), opts);
}

}  // namespace

int cmd_run(const ParsedArgs& args, Context& ctx) {
    if (args.positionals.size() > 1) {
        throw UsageError(std::format("unexpected argument '{}'", args.positionals[1]),
                         "pass arguments for the script or program after '--', e.g. cpm run test -- --verbose");
    }
    std::string profile = profile_option(args);
    if (!args.positionals.empty()) return run_script(args.positionals[0], args, ctx, profile);
    return run_program(args, ctx, profile);
}

}  // namespace cpm::cli
