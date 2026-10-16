#include "install/store.hpp"

#include <format>
#include <random>

#include "core/error.hpp"
#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "install/fetch.hpp"
#include "install/git.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

namespace cpm::install {

std::string entry_name(const lock::Package& p) {
    std::string name = p.key();
    if (const auto* git = std::get_if<lock::GitResolution>(&p.resolution)) name += "+g" + git->commit.substr(0, 12);
    return name;
}

fs::path entry_dir(const fs::path& home, const lock::Package& p) {
    fs::path dir = home / "store" / paths::from_utf8(entry_name(p));
    if (const auto* reg = std::get_if<lock::RegistryResolution>(&p.resolution)) {
        // Same name@version from another registry with different content: keep both (store.md §2.1).
        auto recorded = fsx::try_read_file(dir / ".integrity");
        std::error_code ec;
        if (recorded && fs::exists(dir / ".complete", ec) && std::string(str::trim(*recorded)) != reg->integrity) {
            dir = home / "store" / paths::from_utf8(entry_name(p) + "+s" + reg->integrity.substr(7, 8));
        }
    }
    return dir;
}

fs::path make_temp_dir(const fs::path& home, std::string_view prefix) {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        fs::path dir = home / "tmp" / std::format("{}-{:08x}{:08x}", prefix, rd(), rd());
        std::error_code ec;
        fs::create_directories(dir.parent_path(), ec);
        if (fs::create_directory(dir, ec)) return dir;
    }
    throw Error(std::format("cannot create a temporary directory in {}", paths::to_utf8(home / "tmp")));
}

namespace {

void ensure_layout_version(const fs::path& home) {
    fs::path marker = home / "store" / ".layout-version";
    std::error_code ec;
    if (!fs::exists(marker, ec)) fsx::write_file_atomic(marker, "1\n");
}

// Rejects anything but plain files and directories (registry.md §4.1).
void check_tree(const fs::path& dir, const std::string& what) {
    for (const auto& e : fsx::list_dir(dir)) {
        if (e.is_link) {
            throw Error(std::format("{} contains a symbolic link ({}), which CPM does not support", what, e.name));
        }
        if (e.is_dir) check_tree(dir / paths::from_utf8(e.name), what);
    }
}

json::Value resolution_json(const lock::Resolution& r) {
    json::Value v = json::Value::object();
    if (const auto* git = std::get_if<lock::GitResolution>(&r)) {
        v.set("commit", git->commit);
        if (!git->ref.empty()) v.set("ref", git->ref);
        v.set("type", "git");
        v.set("url", git->url);
    }
    return v;
}

}  // namespace

fs::path ensure_git_entry(const fs::path& home, const lock::Package& p, const std::optional<json::Value>& overlay,
                          bool offline) {
    const auto& git = std::get<lock::GitResolution>(p.resolution);
    fs::path entry = entry_dir(home, p);
    std::error_code ec;
    if (fs::exists(entry / ".complete", ec)) return entry;

    std::string name = entry_name(p);
    FileLock lock = FileLock::acquire(home / "locks" / (Sha256::hex(name).substr(0, 16) + ".lock"), name);
    if (fs::exists(entry / ".complete", ec)) return entry;
    if (fs::exists(entry, ec)) fsx::remove_tree(entry);  // an interrupted earlier attempt
    ensure_layout_version(home);

    auto [mirror, cloned] = git::ensure_mirror(home, git.url, offline);
    if (git::resolve(mirror, git.commit) != git.commit) {
        if (offline || cloned) throw Error(std::format("commit {} of {} is not available", git.commit, git.url));
        git::fetch(mirror, git.url);
        if (git::resolve(mirror, git.commit) != git.commit)
            throw Error(std::format("{} no longer has commit {}", git.url, git.commit),
                        "the lockfile pins a commit that was removed; run 'cpm install' after updating the ref");
    }

    fs::path tmp = make_temp_dir(home, "extract");
    try {
        fs::path tar = tmp / "src.tar";
        git::archive(mirror, git.commit, tar);
        fs::path pkg = tmp / "pkg";
        fsx::create_dirs(pkg);
        auto tar_exe = proc::find_program("tar");
        if (!tar_exe) throw Error("extracting packages needs tar, which was not found on PATH");
        proc::Captured r = proc::capture({.argv = {paths::to_utf8(*tar_exe), "-xf", paths::to_utf8(tar), "-C", paths::to_utf8(pkg)}});
        if (r.exit_code != 0) throw Error(std::format("cannot extract {}:\n{}", name, r.err));
        check_tree(pkg, name);

        fsx::write_file_atomic(pkg / ".integrity", "git:" + git.commit + "\n");
        fsx::write_file_atomic(pkg / ".source.json", json::write(resolution_json(p.resolution)) + "\n");
        if (overlay) fsx::write_file_atomic(pkg / ".cpm-overlay.json", json::write(*overlay, {.sort_keys = true}) + "\n");
        fsx::write_file_atomic(pkg / ".complete", "");
        fsx::create_dirs(entry.parent_path());
        fsx::rename_replace(pkg, entry);
    } catch (...) {
        fsx::remove_tree(tmp);
        throw;
    }
    fsx::remove_tree(tmp);
    return entry;
}

fs::path ensure_registry_entry(const fs::path& home, const lock::Package& p, int strip,
                               const std::optional<json::Value>& overlay, bool allow_http, bool offline) {
    const auto& reg = std::get<lock::RegistryResolution>(p.resolution);
    fs::path entry = entry_dir(home, p);
    std::error_code ec;
    if (fs::exists(entry / ".complete", ec)) return entry;

    std::string name = paths::to_utf8(entry.filename());
    FileLock lock = FileLock::acquire(home / "locks" / (Sha256::hex(name).substr(0, 16) + ".lock"), name);
    if (fs::exists(entry / ".complete", ec)) return entry;
    if (fs::exists(entry, ec)) fsx::remove_tree(entry);  // an interrupted earlier attempt
    ensure_layout_version(home);

    fs::path archive = cached_archive(home, reg.url, reg.integrity, allow_http, offline, p.key());
    fs::path tmp = make_temp_dir(home, "extract");
    try {
        fs::path pkg = tmp / "pkg";
        extract_archive(archive, pkg, strip, p.key());
        fsx::write_file_atomic(pkg / ".integrity", reg.integrity + "\n");
        json::Value source = json::Value::object();
        source.set("integrity", reg.integrity);
        source.set("registry", reg.registry);
        source.set("type", "registry");
        source.set("url", reg.url);
        fsx::write_file_atomic(pkg / ".source.json", json::write(source) + "\n");
        if (overlay) fsx::write_file_atomic(pkg / ".cpm-overlay.json", json::write(*overlay, {.sort_keys = true}) + "\n");
        fsx::write_file_atomic(pkg / ".complete", "");
        fsx::create_dirs(entry.parent_path());
        fsx::rename_replace(pkg, entry);
    } catch (...) {
        fsx::remove_tree(tmp);
        throw;
    }
    fsx::remove_tree(tmp);
    return entry;
}

manifest::Manifest entry_manifest(const fs::path& entry, const lock::Package& p) {
    std::optional<json::Value> overlay;
    if (auto text = fsx::try_read_file(entry / ".cpm-overlay.json")) overlay = json::parse(*text, json::kStrict);
    return manifest::parse_package(fsx::try_read_file(entry / "cpm.jsonc"), overlay, p.name, p.version.to_string(),
                                   entry / "cpm.jsonc", std::format("{} (in the store)", entry_name(p)));
}

}  // namespace cpm::install
