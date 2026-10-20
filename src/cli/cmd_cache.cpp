// cpm cache [clean]: what CPM keeps in $CPM_HOME, and removing what no project uses (docs/spec/cli.md, store.md §8).
#include <algorithm>
#include <format>

#include "cli/commands.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "install/cache.hpp"

namespace cpm::cli {

namespace {

namespace cache = install::cache;
using Kind = cache::Item::Kind;

// "1 package", "2 packages"
std::string count(std::size_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

// "2 packages, 1 archive, 5.4 MB"
std::string summary(const std::vector<cache::Item>& items, bool with_bytes = true) {
    std::size_t n[5] = {};
    std::uintmax_t bytes = 0;
    for (const auto& item : items) {
        ++n[static_cast<int>(item.kind)];
        bytes += item.bytes;
    }
    std::vector<std::string> parts;
    if (n[0]) parts.push_back(count(n[0], "package", "packages"));
    if (n[1]) parts.push_back(count(n[1], "archive", "archives"));
    if (n[2]) parts.push_back(count(n[2], "git mirror", "git mirrors"));
    if (n[3]) parts.push_back(count(n[3], "temporary file", "temporary files"));
    if (n[4]) parts.push_back(count(n[4], "other item", "other items"));
    if (with_bytes) parts.push_back(cache::format_bytes(bytes));
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : ", ") + p;
    return out;
}

void line(std::string_view label, std::string_view value) { log::out(std::format("{:<11}{}\n", label, value)); }

int show(const std::filesystem::path& home) {
    cache::Survey s = cache::survey(home);
    for (const auto& u : s.unreadable) log::warn(std::format("cannot read the cpm-lock.json of {}", u), "cpm cache clean counts it as using nothing");
    std::string where = paths::to_utf8(home);
    std::replace(where.begin(), where.end(), '\\', '/');
    line("home", where);
    line("store", std::format("{}, {}, {}", count(s.packages, "package", "packages"), count(s.libraries, "built library", "built libraries"),
                              cache::format_bytes(s.store_bytes)));
    line("downloads", std::format("{}, {}", count(s.archives, "archive", "archives"), cache::format_bytes(s.download_bytes)));
    line("git", std::format("{}, {}", count(s.mirrors, "mirror", "mirrors"), cache::format_bytes(s.git_bytes)));
    std::string projects = std::to_string(s.projects.size());
    if (!s.gone.empty()) projects += std::format(", and {} that no longer exist", s.gone.size());
    line("projects", projects);
    line("unused", s.unused.empty() ? "nothing" : summary(s.unused) + " (cpm cache clean removes them)");
    if (s.projects.empty()) {
        log::info("note: no projects are registered yet; a project registers when it runs cpm install or cpm build (CPM 0.4.1 and later)");
    }
    return 0;
}

}  // namespace

int cmd_cache(const ParsedArgs& args, Context&) {
    const auto& p = args.positionals;
    bool clean = !p.empty() && p[0] == "clean";
    if (p.size() > 1 || (!p.empty() && !clean)) throw UsageError("usage: cpm cache | cpm cache clean [--dry-run] [--all]");
    bool dry_run = args.flag("dry-run"), all = args.flag("all");
    if (!clean && (dry_run || all)) throw UsageError("--dry-run and --all go with cpm cache clean");
    std::filesystem::path home = paths::cpm_home();
    if (!clean) return show(home);

    cache::CleanResult r = cache::clean(home, all, dry_run);
    for (const auto& root : r.forgotten) log::status(dry_run ? "Would forget" : "Forgot", std::format("{} (no longer exists)", root));
    for (const auto& item : r.removed) {
        log::status(dry_run ? "Would remove" : "Removed", std::format("{} ({})", item.label, cache::format_bytes(item.bytes)));
    }
    for (const auto& item : r.busy) log::warn(std::format("kept {}: another cpm process is using it", item.label), "run cpm cache clean again later");
    if (r.removed.empty() && r.forgotten.empty()) {
        log::info(all ? "The store and caches are already empty" : "Nothing to clean: every package, archive and mirror is in use");
    } else if (!r.removed.empty()) {
        log::status(dry_run ? "Would free" : "Freed", std::format("{} ({})", cache::format_bytes(r.bytes), summary(r.removed, false)));
    }
    return 0;
}

}  // namespace cpm::cli
