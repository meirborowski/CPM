#include "install/git.hpp"

#include <format>
#include <random>

#include "core/error.hpp"
#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "semver/version.hpp"

namespace cpm::install::git {

namespace {

const fs::path& git_program() {
    static const fs::path git = [] {
        auto p = proc::find_program("git");
        if (!p) throw Error("git dependencies need git, which was not found on PATH", "install git (https://git-scm.com)");
        return *p;
    }();
    return git;
}

// Runs git without prompts and with stable line endings; returns the captured result.
proc::Captured run(std::vector<std::string> args) {
    std::vector<std::string> argv = {paths::to_utf8(git_program()), "-c", "core.autocrlf=false", "-c", "core.eol=lf"};
    argv.insert(argv.end(), args.begin(), args.end());
    return proc::capture({.argv = argv, .env_set = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_ASKPASS", ""}}});
}

std::string first_lines(std::string_view text) {
    auto lines = str::split_lines(text);
    if (lines.size() > 12) lines.resize(12);
    return str::join(lines, "\n");
}

std::string random_suffix() {
    std::random_device rd;
    return std::format("{:08x}", rd());
}

}  // namespace

fs::path mirror_dir(const fs::path& home, std::string_view url) {
    return home / "cache" / "git" / (Sha256::hex(url).substr(0, 16) + ".git");
}

std::pair<fs::path, bool> ensure_mirror(const fs::path& home, std::string_view url, bool offline) {
    fs::path dir = mirror_dir(home, url);
    std::error_code ec;
    if (fs::exists(dir / "HEAD", ec)) return {dir, false};
    if (offline) throw Error(std::format("{} has not been fetched before, and --offline is set", url));

    FileLock lock = FileLock::acquire(home / "locks" / ("git-" + dir.stem().string() + ".lock"), std::string(url));
    if (fs::exists(dir / "HEAD", ec)) return {dir, false};  // another process cloned it meanwhile

    log::status("Fetching", std::string(url));
    fs::path tmp = home / "tmp" / ("clone-" + random_suffix());
    fsx::create_dirs(tmp.parent_path());
    proc::Captured r = run({"clone", "--mirror", "--quiet", std::string(url), paths::to_utf8(tmp)});
    if (r.exit_code != 0) {
        fsx::remove_tree(tmp);
        throw Error(std::format("cannot clone {}:\n{}", url, first_lines(r.err)), "check the URL and your network access");
    }
    fsx::create_dirs(dir.parent_path());
    fsx::rename_replace(tmp, dir);
    return {dir, true};
}

void fetch(const fs::path& mirror, std::string_view url) {
    log::status("Fetching", std::string(url));
    proc::Captured r = run({"-C", paths::to_utf8(mirror), "fetch", "--quiet", "--prune", "--force", "origin"});
    if (r.exit_code != 0) throw Error(std::format("cannot fetch {}:\n{}", url, first_lines(r.err)));
}

std::optional<std::string> resolve(const fs::path& mirror, std::string_view ref) {
    std::string spec = ref.empty() ? std::string("HEAD") : std::string(ref);
    proc::Captured r = run({"-C", paths::to_utf8(mirror), "rev-parse", "--verify", "--quiet", spec + "^{commit}"});
    if (r.exit_code != 0) return std::nullopt;
    std::string commit(str::trim(r.out));
    if (commit.size() != 40) return std::nullopt;
    return commit;
}

std::optional<std::string> show_file(const fs::path& mirror, std::string_view commit, std::string_view path) {
    proc::Captured r = run({"-C", paths::to_utf8(mirror), "show", std::format("{}:{}", commit, path)});
    if (r.exit_code != 0) return std::nullopt;
    return r.out;
}

void archive(const fs::path& mirror, std::string_view commit, const fs::path& tar_file) {
    proc::Captured r = run({"-C", paths::to_utf8(mirror), "archive", "--format=tar", "-o", paths::to_utf8(tar_file),
                            std::string(commit)});
    if (r.exit_code != 0) throw Error(std::format("git archive of {} failed:\n{}", commit, first_lines(r.err)));
}

std::optional<std::string> version_from_tag(std::string_view ref) {
    std::string_view v = ref;
    if (v.starts_with('v')) v.remove_prefix(1);
    if (semver::Version::parse(v)) return std::string(v);
    return std::nullopt;
}

}  // namespace cpm::install::git
