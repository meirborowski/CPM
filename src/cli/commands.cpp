#include "cli/commands.hpp"

#include <format>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/term.hpp"
#include "version.hpp"

namespace cpm::cli {

namespace fs = std::filesystem;

namespace {

using Handler = int (*)(const ParsedArgs&, Context&);

const OptionSpec kProfileOption{"profile", 0, true, "<name>", "Build profile (default: debug)"};
const OptionSpec kReleaseOption{"release", 0, false, "", "Same as --profile release"};
const OptionSpec kJobsOption{"jobs", 'j', true, "<n>", "Parallel jobs passed to Ninja"};
const OptionSpec kCxxOption{"cxx", 0, true, "<compiler>", "C++ compiler to use (name or path)"};

const std::vector<OptionSpec> kGlobalOptions = {
    {"dir", 'C', true, "<path>", "Run as if started in <path>"},
    {"verbose", 'v', false, "", "More output; -vv also prints subprocess command lines"},
    {"quiet", 'q', false, "", "Print errors only"},
    {"color", 0, true, "<when>", "Colour output: auto, always or never"},
    {"help", 'h', false, "", "Show help"},
    {"version", 'V', false, "", "Show the CPM version"},
};

struct Command {
    CommandSpec spec;
    Handler handler;              // nullptr: not implemented yet
    std::string_view milestone;   // when it is planned, for unimplemented commands
};

const std::vector<Command>& command_table() {
    static const std::vector<Command> table = {
        {{"init", {}, "Create a new package", "cpm init [<dir>] [options]",
          {{"yes", 'y', false, "", "Accept all defaults"},
           {"name", 0, true, "<name>", "Package name"},
           {"lib", 0, false, "", "Create a static library"},
           {"shared-lib", 0, false, "", "Create a shared library"},
           {"header-only", 0, false, "", "Create a header-only library"},
           {"std", 0, true, "<std>", "C++ standard (default: c++23)"},
           {"force", 0, false, "", "Overwrite an existing cpm.jsonc"}}},
         &cmd_init, ""},
        {{"install", {"i", "add"}, "Install dependencies", "cpm install [<spec>...] [options]",
          {{"save-dev", 'D', false, "", "Save to devDependencies"},
           {"save-exact", 'E', false, "", "Save the exact version"},
           {"frozen-lockfile", 0, false, "", "Fail if cpm-lock.json is not fresh"},
           {"offline", 0, false, "", "Use only the local caches"},
           {"prefer-offline", 0, false, "", "Prefer cached registry data"},
           {"registry", 0, true, "<name|url>", "Registry for the given specs"},
           kProfileOption, kReleaseOption, kCxxOption,
           {"no-build", 0, false, "", "Do not build dependency binaries"}}},
         &cmd_install, ""},
        {{"remove", {"rm", "uninstall"}, "Remove dependencies", "cpm remove <name>... [options]",
          {{"offline", 0, false, "", "Use only the local caches"}, kProfileOption, kReleaseOption, kCxxOption}},
         &cmd_remove, ""},
        {{"build", {}, "Compile the package", "cpm build [options]",
          {kProfileOption, kReleaseOption, kJobsOption,
           {"tests", 0, false, "", "Also build the test executables"},
           kCxxOption,
           {"clean", 0, false, "", "Delete the profile's build directory first"}}},
         &cmd_build, ""},
        {{"run", {}, "Run a script or the built executable", "cpm run [<script>] [options] [-- <args>...]",
          {kProfileOption, kReleaseOption, kJobsOption, kCxxOption}},
         &cmd_run, ""},
        {{"test", {}, "Build and run the tests", "cpm test [<filter>...] [options]",
          {kProfileOption, kReleaseOption, kJobsOption, kCxxOption,
           {"timeout", 0, true, "<seconds>", "Per-test timeout (default: 300)"},
           {"no-run", 0, false, "", "Build the tests without running them"}}},
         &cmd_test, ""},
        {{"publish", {}, "Publish the package to a registry", "cpm publish [options]",
          {{"registry", 0, true, "<name>", "Target registry"},
           {"out", 0, true, "<dir>", "Write the archive and index entry here"},
           {"with-binary", 0, false, "", "Also publish a prebuilt binary"},
           {"dry-run", 0, false, "", "Show what would be published"}}},
         nullptr, "post-MVP"},
    };
    return table;
}

std::vector<CommandSpec> command_specs() {
    std::vector<CommandSpec> specs;
    for (const auto& c : command_table()) specs.push_back(c.spec);
    return specs;
}

const Command& find(const CommandSpec* spec) {
    for (const auto& c : command_table()) {
        if (c.spec.name == spec->name) return c;
    }
    throw Error("internal error: unknown command");
}

std::string general_help() {
    std::string text = std::format("cpm {} - C++ package manager and build system\n\n", version);
    text += "Usage: cpm [options] <command> [args]\n\nCommands:\n";
    for (const auto& c : command_table()) {
        std::string name(c.spec.name);
        std::string summary(c.spec.summary);
        if (!c.spec.aliases.empty()) {
            std::vector<std::string> aliases(c.spec.aliases.begin(), c.spec.aliases.end());
            summary += " (aliases: ";
            for (std::size_t i = 0; i < aliases.size(); ++i) summary += (i ? ", " : "") + aliases[i];
            summary += ")";
        }
        if (c.handler == nullptr) summary += std::format(" [planned: {}]", c.milestone);
        text += std::format("  {:<10} {}\n", name, summary);
    }
    text += "\nOptions:\n" + format_options(kGlobalOptions);
    text += "\nRun 'cpm <command> --help' for the options of a command.\n";
    return text;
}

std::string command_help(const CommandSpec& spec) {
    std::string text = std::format("{}\n\nUsage: {}\n", spec.summary, spec.usage);
    if (!spec.options.empty()) text += "\nOptions:\n" + format_options(spec.options);
    text += "\nGlobal options:\n" + format_options(kGlobalOptions);
    return text;
}

std::string version_text(bool verbose) {
    std::string text = std::format("cpm {}\n", version);
    if (verbose) {
#if defined(__clang__)
        text += std::format("compiler: clang {}\n", __clang_version__);
#elif defined(__GNUC__)
        text += std::format("compiler: gcc {}\n", __VERSION__);
#elif defined(_MSC_VER)
        text += std::format("compiler: msvc {}\n", _MSC_FULL_VER);
#endif
        try {
            text += std::format("home: {}\n", paths::to_utf8(paths::cpm_home()));
        } catch (const Error&) {
            text += "home: (unknown)\n";
        }
    }
    return text;
}

ColorMode parse_color(const std::optional<std::string>& value) {
    if (!value || *value == "auto") return ColorMode::Auto;
    if (*value == "always") return ColorMode::Always;
    if (*value == "never") return ColorMode::Never;
    throw UsageError(std::format("invalid value '{}' for --color", *value), "use auto, always or never");
}

bool decide_color(ColorMode mode) {
    switch (mode) {
        case ColorMode::Always: return true;
        case ColorMode::Never: return false;
        case ColorMode::Auto: break;
    }
    if (env::get("NO_COLOR").has_value()) return false;
    if (auto term = env::get("TERM"); term && *term == "dumb") return false;
    return term::is_terminal(2);
}

// The previous executable renamed aside by a self-rebuild (build.md §6).
void remove_stale_self() {
    try {
        fs::path self = paths::current_exe();
        fs::path old = self;
        old += ".old";
        std::error_code ec;
        fs::remove(old, ec);
    } catch (...) {
    }
}

}  // namespace

int run(const std::vector<std::string>& argv) {
    term::init();
    remove_stale_self();

    Context ctx;
    try {
        std::vector<CommandSpec> specs = command_specs();
        ParsedArgs args = parse_args(argv, kGlobalOptions, specs);

        if (args.flag("quiet")) log::set_level(log::Level::Quiet);
        if (int v = args.count("verbose"); v > 0) log::set_level(v > 1 ? log::Level::Trace : log::Level::Verbose);
        if (auto lvl = env::get("CPM_LOG"); lvl && (*lvl == "debug" || *lvl == "trace"))
            log::set_level(log::Level::Trace);
        ctx.color_mode = parse_color(args.value("color"));
        log::set_color(decide_color(ctx.color_mode));

        if (args.flag("version")) {
            log::out(version_text(log::verbose()));
            return 0;
        }
        if (args.command == nullptr) {
            log::out(general_help());
            return args.flag("help") ? 0 : 2;
        }
        if (args.flag("help")) {
            log::out(command_help(*args.command));
            return 0;
        }

        ctx.cwd = fs::current_path();
        if (auto dir = args.value("dir")) {
            fs::path p = paths::from_utf8(*dir);
            ctx.cwd = fs::absolute(p).lexically_normal();
            std::error_code ec;
            if (!fs::is_directory(ctx.cwd, ec)) throw UsageError(std::format("-C: '{}' is not a directory", *dir));
        }

        const Command& command = find(args.command);
        if (command.handler == nullptr) {
            throw Error(std::format("'cpm {}' is not implemented yet (planned for {})", command.spec.name,
                                    command.milestone),
                        "see docs/spec/architecture.md §9 for the milestones");
        }
        return command.handler(args, ctx);
    } catch (const UsageError& e) {
        log::error(e.what(), e.hint());
        return 2;
    } catch (const DiagnosticsError& e) {
        for (const auto& d : e.diagnostics()) log::diagnostic(d);
        return 1;
    } catch (const Error& e) {
        log::error(e.what(), e.hint());
        return 1;
    } catch (const std::exception& e) {
        log::error(std::format("internal error: {}", e.what()),
                   "this is a bug in CPM; please report it with the command you ran");
        return 1;
    }
}

}  // namespace cpm::cli
