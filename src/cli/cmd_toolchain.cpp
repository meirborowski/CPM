// cpm toolchain list / use: the compilers on this machine, and the machine's choice (docs/spec/cli.md, build.md §1).
#include <algorithm>
#include <format>

#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"
#include "manifest/config.hpp"
#include "manifest/manifest.hpp"
#include "toolchain/select.hpp"

namespace cpm::cli {

namespace {

namespace fs = std::filesystem;

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

// What a build in `cwd` would choose: the project's "toolchain" (when there is a project) and the machine's choice.
toolchain::Selection selection_here(const fs::path& cwd, const manifest::UserConfig& config, const fs::path& home) {
    toolchain::Selection sel{.config_cxx = config.cxx, .config_toolchain = config.toolchain, .home = home};
    if (auto root = paths::find_project_root(cwd)) {
        try {
            sel.project = manifest::load(*root / "cpm.jsonc", cwd).toolchain;
        } catch (const Error&) {
            // An invalid manifest: list the compilers anyway, as the machine would choose.
        }
    }
    return sel;
}

int list(const fs::path& cwd, const fs::path& home) {
    manifest::UserConfig config = load_config();
    std::vector<toolchain::Found> found = toolchain::list_compilers(home);
    std::optional<toolchain::Toolchain> chosen;
    std::string why_not;
    try {
        chosen = toolchain::find_toolchain(selection_here(cwd, config, home));
    } catch (const Error& e) {
        why_not = e.what();
    }
    // A compiler chosen by CXX or config.jsonc may live where CPM doesn't look: list it too.
    if (chosen && std::none_of(found.begin(), found.end(), [&](const toolchain::Found& f) { return same_file(f.cxx, chosen->cxx); })) {
        found.push_back({chosen->cxx, chosen, "", ""});
    }
    if (found.empty()) {
        log::info("No C++ compilers found");
        log::info("note: run cpm setup to install one");
        return 0;
    }

    std::size_t family_width = 6, version_width = 7;
    for (const auto& f : found) {
        if (!f.toolchain) continue;
        family_width = std::max(family_width, toolchain::to_string(f.toolchain->family).size());
        version_width = std::max(version_width, f.toolchain->version.size());
    }
    log::out(std::format("  {:<{}}  {:<{}}  path\n", "family", family_width, "version", version_width));
    for (const auto& f : found) {
        bool star = chosen && same_file(f.cxx, chosen->cxx);
        std::string family = f.toolchain ? std::string(toolchain::to_string(f.toolchain->family)) : "?";
        std::string version = f.toolchain ? f.toolchain->version : "?";
        std::string extra = !f.problem.empty() ? "  (" + f.problem + ")" : !f.note.empty() ? "  (" + f.note + ")" : "";
        log::out(std::format("{} {:<{}}  {:<{}}  {}{}\n", star ? "*" : " ", family, family_width, version, version_width,
                             paths::to_utf8(f.cxx), extra));
    }
    if (chosen) log::info(std::format("note: * is what cpm build uses here: {}, {}", chosen->description(), chosen->chosen_by));
    else log::info(std::format("note: cpm build can't choose a compiler here: {}", why_not));
    return 0;
}

int use(const std::string& choice, const fs::path& home) {
    if (choice == "auto") {
        manifest::set_user_config(home, "toolchain", std::nullopt);
        manifest::set_user_config(home, "cxx", std::nullopt);
        log::status("Removed", "this machine's compiler choice from config.jsonc: builds choose automatically, unless the "
                               "command line, CXX or a project's \"toolchain\" choose");
        return 0;
    }
    // Check the choice first, so that a typo fails now rather than at the next build.
    toolchain::Toolchain tc = toolchain::find_toolchain({.command = toolchain::Choice{choice, "cpm toolchain use"}, .home = home});
    bool request = toolchain::parse_request(choice).has_value();
    manifest::set_user_config(home, request ? "toolchain" : "cxx", choice);
    manifest::set_user_config(home, request ? "cxx" : "toolchain", std::nullopt);
    log::status("Using", std::format("{} ({}) for builds on this machine", tc.description(), paths::to_utf8(tc.cxx)));
    if (env::is_set("CXX")) {
        log::warn(std::format("CXX is set to {}, which comes first", *env::get("CXX")), "unset CXX to use this machine's choice");
    } else {
        log::info("note: --toolchain and a project's \"toolchain\" in cpm.jsonc still come first");
    }
    return 0;
}

}  // namespace

int cmd_toolchain(const ParsedArgs& args, Context& ctx) {
    const auto& p = args.positionals;
    std::string action = p.empty() ? "" : p[0];
    if (action == "list" && p.size() == 1) return list(ctx.cwd, paths::cpm_home());
    if (action == "use" && p.size() >= 2) {
        std::vector<std::string> rest(p.begin() + 1, p.end());
        return use(str::join(rest, " "), paths::cpm_home());
    }
    throw UsageError("usage: cpm toolchain list | cpm toolchain use <choice> | cpm toolchain use auto",
                     "a choice is a request such as clang or \"gcc >=14\" (quote ranges), or a compiler name or path");
}

}  // namespace cpm::cli
