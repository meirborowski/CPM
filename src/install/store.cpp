#include "install/store.hpp"

#include <algorithm>
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

// CPM's own files at the top of an entry (§2.1). Everything else outside lib/ came from the package.
bool is_cpm_file(std::string_view name) {
    return name == ".complete" || name == ".integrity" || name == ".source.json" || name == ".cpm-overlay.json";
}

std::vector<fsx::DirEntry> sorted_list(const fs::path& dir) {
    std::vector<fsx::DirEntry> entries = fsx::list_dir(dir);
    std::ranges::sort(entries, {}, &fsx::DirEntry::name);
    return entries;
}

// The path, relative to `root`, of the first file in the directory `rel` or below it (files before
// subdirectories), or nullopt when that tree holds no file.
std::optional<std::string> first_file(const fs::path& root, const std::string& rel) {
    std::vector<fsx::DirEntry> entries = sorted_list(root / paths::from_utf8(rel));
    for (const auto& e : entries) {
        if (e.is_file) return rel + "/" + e.name;
    }
    for (const auto& e : entries) {
        if (!e.is_dir) continue;
        if (auto f = first_file(root, rel + "/" + e.name)) return f;
    }
    return std::nullopt;
}

// The content of .complete for an extracted package (§2.3): a sample of its files, which entry_intact checks
// on every reuse. One file from each top-level directory, then the top-level files, at most 32 in all.
std::string complete_marker(const fs::path& pkg) {
    std::vector<fsx::DirEntry> entries = sorted_list(pkg);
    std::vector<std::string> sample;
    for (const auto& e : entries) {
        if (!e.is_dir) continue;
        if (auto f = first_file(pkg, e.name)) sample.push_back(*f);
    }
    for (const auto& e : entries) {
        if (e.is_file && !is_cpm_file(e.name)) sample.push_back(e.name);
    }
    std::string out = "# files checked whenever this entry is reused (docs/spec/store.md, section 2.3)\n";
    std::size_t n = 0;
    for (const auto& path : sample) {
        if (path.starts_with('#') || path.find_first_of("\r\n") != std::string::npos) continue;
        if (++n > 32) break;
        out += path + "\n";
    }
    return out;
}

// Entries made before .complete listed files: at least one of the package's files must be left.
bool has_package_file(const fs::path& entry) {
    for (const auto& e : fsx::list_dir(entry)) {
        if (e.is_file && !is_cpm_file(e.name)) return true;
        if (e.is_dir && e.name != "lib" && first_file(entry, e.name)) return true;
    }
    return false;
}

// Clears the way for a fresh extraction, under the entry's lock. An entry without .complete is an
// interrupted attempt; a complete one that failed entry_intact lost files to something outside CPM.
void discard(const fs::path& entry, std::string_view name) {
    std::error_code ec;
    if (fs::exists(entry / ".complete", ec)) {
        log::warn(std::format("the store entry {} is missing files; extracting it again", name));
    }
    if (fs::exists(entry, ec)) fsx::remove_tree(entry);
}

}  // namespace

bool entry_intact(const fs::path& entry) {
    auto marker = fsx::try_read_file(entry / ".complete");
    if (!marker) return false;
    if (marker->empty()) return has_package_file(entry);
    for (const auto& line : str::split_lines(*marker)) {
        if (line.empty() || line.starts_with('#')) continue;
        std::error_code ec;
        if (!fs::is_regular_file(entry / paths::from_utf8(line), ec)) return false;
    }
    return true;
}

bool library_intact(const fs::path& lib_dir) {
    std::error_code ec;
    if (!fs::exists(lib_dir / ".complete", ec)) return false;
    auto text = fsx::try_read_file(lib_dir / "meta.json");
    if (!text) return false;
    try {
        json::Value meta = json::parse(*text, json::kStrict);
        const json::Value* artifacts = meta.find("artifacts");
        if (artifacts == nullptr || !artifacts->is_array()) return true;
        for (const auto& artifact : artifacts->as_array()) {
            const json::Value* file = artifact.is_object() ? artifact.find("file") : nullptr;
            if (file && file->is_string() && !fs::is_regular_file(lib_dir / paths::from_utf8(file->as_string()), ec))
                return false;
        }
    } catch (const std::exception&) {
        return false;  // a truncated meta.json
    }
    return true;
}

fs::path ensure_git_entry(const fs::path& home, const lock::Package& p, const std::optional<json::Value>& overlay,
                          bool offline) {
    const auto& git = std::get<lock::GitResolution>(p.resolution);
    fs::path entry = entry_dir(home, p);
    if (entry_intact(entry)) return entry;

    std::string name = entry_name(p);
    FileLock lock = FileLock::acquire(home / "locks" / (Sha256::hex(name).substr(0, 16) + ".lock"), name);
    if (entry_intact(entry)) return entry;
    discard(entry, name);
    ensure_layout_version(home);

    auto [mirror, cloned] = git::ensure_mirror(home, git.url, offline);
    if (git::resolve(mirror, git.commit) != git.commit) {
        if (offline || cloned) throw Error(std::format("commit {} of {} is not available", git.commit, git.url));
        git::fetch(home, git.url);
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

        std::string marker = complete_marker(pkg);
        fsx::write_file_atomic(pkg / ".integrity", "git:" + git.commit + "\n");
        fsx::write_file_atomic(pkg / ".source.json", json::write(resolution_json(p.resolution)) + "\n");
        if (overlay) fsx::write_file_atomic(pkg / ".cpm-overlay.json", json::write(*overlay, {.sort_keys = true}) + "\n");
        fsx::write_file_atomic(pkg / ".complete", marker);
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
    if (entry_intact(entry)) return entry;

    std::string name = paths::to_utf8(entry.filename());
    FileLock lock = FileLock::acquire(home / "locks" / (Sha256::hex(name).substr(0, 16) + ".lock"), name);
    if (entry_intact(entry)) return entry;
    discard(entry, name);
    ensure_layout_version(home);

    fs::path archive = cached_archive(home, reg.url, reg.integrity, allow_http, offline, p.key());
    fs::path tmp = make_temp_dir(home, "extract");
    try {
        fs::path pkg = tmp / "pkg";
        extract_archive(archive, pkg, strip, p.key());
        std::string marker = complete_marker(pkg);
        fsx::write_file_atomic(pkg / ".integrity", reg.integrity + "\n");
        json::Value source = json::Value::object();
        source.set("integrity", reg.integrity);
        source.set("registry", reg.registry);
        source.set("type", "registry");
        source.set("url", reg.url);
        fsx::write_file_atomic(pkg / ".source.json", json::write(source) + "\n");
        if (overlay) fsx::write_file_atomic(pkg / ".cpm-overlay.json", json::write(*overlay, {.sort_keys = true}) + "\n");
        fsx::write_file_atomic(pkg / ".complete", marker);
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
