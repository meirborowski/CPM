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

// Runs git on a mirror. --git-dir rather than -C: if the mirror is damaged, git must fail instead of finding
// a repository in a parent directory (say, a dotfiles repository around $CPM_HOME) and using that.
proc::Captured run_in(const fs::path& mirror, std::vector<std::string> args) {
    args.insert(args.begin(), "--git-dir=" + paths::to_utf8(mirror));
    return run(std::move(args));
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

namespace {

FileLock lock_mirror(const fs::path& home, const fs::path& mirror, std::string_view url) {
    return FileLock::acquire(home / "locks" / ("git-" + mirror.stem().string() + ".lock"), std::string(url));
}

// Clones `url` into `mirror`, staged in $CPM_HOME/tmp. Whatever was at `mirror` (a damaged mirror) is
// replaced only once the clone has worked. The caller holds the mirror's lock.
void clone(const fs::path& home, std::string_view url, const fs::path& mirror) {
    fs::path tmp = home / "tmp" / ("clone-" + random_suffix());
    fsx::create_dirs(tmp.parent_path());
    proc::Captured r = run({"clone", "--mirror", "--quiet", std::string(url), paths::to_utf8(tmp)});
    if (r.exit_code != 0) {
        fsx::remove_tree(tmp);
        throw Error(std::format("cannot clone {}:\n{}", url, first_lines(r.err)), "check the URL and your network access");
    }
    std::error_code ec;
    if (fs::exists(mirror, ec)) fsx::remove_tree(mirror);
    fsx::create_dirs(mirror.parent_path());
    fsx::rename_replace(tmp, mirror);
}

// Whether git can still use the mirror: it is a repository, the objects its refs point to exist, and its
// remote is `url`. Cheap: no history is walked (store.md §3).
bool usable(const fs::path& mirror, std::string_view url) {
    proc::Captured origin = run_in(mirror, {"config", "--get", "remote.origin.url"});
    if (origin.exit_code != 0 || str::trim(origin.out) != url) return false;
    return run_in(mirror, {"for-each-ref", "--format=%(objecttype)"}).exit_code == 0;
}

}  // namespace

std::pair<fs::path, bool> ensure_mirror(const fs::path& home, std::string_view url, bool offline) {
    fs::path dir = mirror_dir(home, url);
    std::error_code ec;
    if (fs::exists(dir / "HEAD", ec)) return {dir, false};
    if (offline) throw Error(std::format("{} has not been fetched before, and --offline is set", url));

    FileLock lock = lock_mirror(home, dir, url);
    if (fs::exists(dir / "HEAD", ec)) return {dir, false};  // another process cloned it meanwhile
    if (fs::exists(dir, ec)) log::warn(std::format("the git mirror of {} is damaged; cloning it again", url));
    log::status("Fetching", std::string(url));
    clone(home, url, dir);
    return {dir, true};
}

void fetch(const fs::path& home, std::string_view url) {
    fs::path mirror = mirror_dir(home, url);
    log::status("Fetching", std::string(url));
    proc::Captured r = run_in(mirror, {"fetch", "--quiet", "--prune", "--force", "origin"});
    if (r.exit_code == 0) return;
    // A network or remote problem leaves the mirror alone. A mirror git can no longer use (files deleted
    // by something outside CPM) is cloned again.
    if (usable(mirror, url)) throw Error(std::format("cannot fetch {}:\n{}", url, first_lines(r.err)));
    FileLock lock = lock_mirror(home, mirror, url);
    if (usable(mirror, url)) return;  // another process cloned it again meanwhile
    log::warn(std::format("the git mirror of {} is damaged; cloning it again", url));
    clone(home, url, mirror);
}

std::optional<std::string> resolve(const fs::path& mirror, std::string_view ref) {
    std::string spec = ref.empty() ? std::string("HEAD") : std::string(ref);
    proc::Captured r = run_in(mirror, {"rev-parse", "--verify", "--quiet", spec + "^{commit}"});
    if (r.exit_code != 0) return std::nullopt;
    std::string commit(str::trim(r.out));
    if (commit.size() != 40) return std::nullopt;
    return commit;
}

std::optional<std::string> show_file(const fs::path& mirror, std::string_view commit, std::string_view path) {
    proc::Captured r = run_in(mirror, {"show", std::format("{}:{}", commit, path)});
    if (r.exit_code != 0) return std::nullopt;
    return r.out;
}

void archive(const fs::path& mirror, std::string_view commit, const fs::path& tar_file) {
    proc::Captured r = run_in(mirror, {"archive", "--format=tar", "-o", paths::to_utf8(tar_file), std::string(commit)});
    if (r.exit_code != 0) throw Error(std::format("git archive of {} failed:\n{}", commit, first_lines(r.err)));
}

std::optional<std::string> version_from_tag(std::string_view ref) {
    std::string_view v = ref;
    if (v.starts_with('v')) v.remove_prefix(1);
    if (semver::Version::parse(v)) return std::string(v);
    return std::nullopt;
}

}  // namespace cpm::install::git
