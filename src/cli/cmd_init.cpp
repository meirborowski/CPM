#include <algorithm>
#include <format>

#include "cli/commands.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/term.hpp"
#include "init/init.hpp"

namespace cpm::cli {

namespace fs = std::filesystem;

int cmd_init(const ParsedArgs& args, Context& ctx) {
    if (args.positionals.size() > 1) {
        throw UsageError(std::format("unexpected argument '{}'", args.positionals[1]), "usage: cpm init [<dir>] [options]");
    }
    init::Options o;
    o.dir = args.positionals.empty() ? ctx.cwd : fs::absolute(ctx.cwd / paths::from_utf8(args.positionals[0]));
    o.dir = o.dir.lexically_normal();
    if (!o.dir.has_filename()) o.dir = o.dir.parent_path();  // "dir/" -> "dir"
    o.force = args.flag("force");

    init::Fixed fixed;
    int kinds = args.count("lib") + args.count("shared-lib") + args.count("header-only");
    if (kinds > 1) throw UsageError("use only one of --lib, --shared-lib and --header-only");
    if (args.flag("lib")) o.kind = manifest::TargetKind::StaticLib;
    if (args.flag("shared-lib")) o.kind = manifest::TargetKind::SharedLib;
    if (args.flag("header-only")) o.kind = manifest::TargetKind::HeaderOnly;
    fixed.kind = kinds == 1;

    if (auto name = args.value("name")) {
        if (!manifest::valid_package_name(*name)) {
            throw UsageError(std::format("invalid package name '{}'", *name),
                             "use 1-64 lowercase letters, digits, '.', '_' or '-', starting and ending with a letter or digit");
        }
        o.name = *name;
        fixed.name = true;
    } else {
        o.name = init::name_from_directory(paths::to_utf8(o.dir.filename()));
    }
    if (auto std_value = args.value("std")) {
        const auto& stds = manifest::known_standards();
        if (std::find(stds.begin(), stds.end(), *std_value) == stds.end()) {
            throw UsageError(std::format("unknown C++ standard '{}'", *std_value),
                             "use c++11, c++14, c++17, c++20, c++23 or c++26");
        }
        o.std = *std_value;
        fixed.std = true;
    }

    // Interactive unless -y, or when there is nobody to answer (stdin is not a terminal).
    bool interactive = !args.flag("yes") && term::is_terminal(0);
    if (interactive) {
        std::error_code ec;
        if (fs::exists(o.dir / "cpm.jsonc", ec) && !o.force) init::write(o);  // fails with the usual message
        log::info(std::format("Creating a package in {}. Press Enter to accept the default in parentheses.",
                              paths::display(o.dir, ctx.cwd)));
        init::ask_questions(
            o, fixed,
            [](const std::string& prompt) {
                log::err(prompt);
                return term::read_line();
            },
            [](const std::string& message) { log::err(std::format("  {}\n", log::yellow(message))); });
    } else if (o.name.empty()) {
        throw UsageError(std::format("cannot derive a package name from the directory name '{}'",
                                     paths::to_utf8(o.dir.filename())),
                         "pass --name <name>");
    }

    init::Result r = init::write(o);

    std::string where = paths::display(o.dir, ctx.cwd);
    if (where.empty() || where == ".") where = "the current directory";
    log::status("Created", std::format("{} package '{}' in {}", manifest::to_string(o.kind), o.name, where));
    for (const auto& f : r.created) {
        log::info(std::format("{:>12} {}{}", "", f, f == "cpm.jsonc" && r.overwrote_manifest ? " (replaced)" : ""));
    }
    if (r.gitignore_updated) log::info(std::format("{:>12} .gitignore (added CPM's entries)", ""));
    for (const auto& f : r.skipped) log::status("Kept", std::format("{} (already exists)", f));

    std::vector<std::pair<std::string, std::string>> steps;
    if (paths::display(o.dir, ctx.cwd) != "." && o.dir != ctx.cwd) {
        steps.emplace_back(std::format("cd {}", proc::quote(paths::display(o.dir, ctx.cwd))), "");
    }
    if (o.kind == manifest::TargetKind::Executable) steps.emplace_back("cpm run", "build and run it");
    else steps.emplace_back("cpm build", "build it");
    steps.emplace_back("cpm test", "build and run the tests in tests/");
    log::info("\nNext:");
    for (const auto& [cmd, what] : steps) {
        log::info(what.empty() ? std::format("  {}", cmd) : std::format("  {:<12} {}", cmd, what));
    }
    return 0;
}

}  // namespace cpm::cli
