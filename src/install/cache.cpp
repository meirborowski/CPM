#include "install/cache.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <set>
#include <variant>

#include "core/error.hpp"
#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "install/git.hpp"
#include "install/store.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "lock/lockfile.hpp"

namespace cpm::install::cache {

namespace {

constexpr auto kStaleTemporary = std::chrono::hours(24);

fs::path projects_file(const fs::path& home) { return home / "projects.json"; }

// locks/<sha256(key)[:16]>.lock (store.md §4)
fs::path lock_file(const fs::path& home, std::string_view key) {
    return home / "locks" / (Sha256::hex(key).substr(0, 16) + ".lock");
}

// The UTC date, "2026-10-20".
std::string today() {
    std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()));
}

// projects.json's projects; empty when the file is missing or unreadable.
std::map<std::string, std::string> read_projects(const fs::path& home) {
    std::map<std::string, std::string> out;
    auto text = fsx::try_read_file(projects_file(home));
    if (!text) return out;
    json::Value doc;
    try {
        doc = json::parse(*text, json::kStrict);
    } catch (const Error&) {
        return out;
    }
    const json::Value* list = doc.find("projects");
    if (list == nullptr || !list->is_object()) return out;
    for (const auto& m : list->as_object()) {
        const json::Value* last = m.value.is_object() ? m.value.find("lastUsed") : nullptr;
        out[m.key] = last != nullptr && last->is_string() ? last->as_string() : "";
    }
    return out;
}

void write_projects(const fs::path& home, const std::map<std::string, std::string>& list) {
    json::Value entries = json::Value::object();
    for (const auto& [root, last] : list) {
        json::Value p = json::Value::object();
        p.set("lastUsed", last);
        entries.set(root, std::move(p));
    }
    json::Value doc = json::Value::object();
    doc.set("projectsVersion", 1);
    doc.set("projects", std::move(entries));
    fsx::write_file_atomic(projects_file(home), json::write(doc) + "\n");
}

FileLock lock_projects(const fs::path& home) { return FileLock::acquire(lock_file(home, "projects.json"), "projects.json"); }

bool project_exists(const std::string& root) {
    std::error_code ec;
    return fs::is_regular_file(paths::from_utf8(root) / "cpm.jsonc", ec);
}

// The bytes of a file, or of every file in a directory tree. Links are not followed.
std::uintmax_t size_of(const fs::path& p) {
    std::error_code ec;
    if (fsx::is_link(p)) return 0;
    if (fs::is_regular_file(p, ec)) {
        auto n = fs::file_size(p, ec);
        return ec ? 0 : n;
    }
    std::uintmax_t total = 0;
    for (const auto& e : fsx::list_dir(p)) {
        if (e.is_link) continue;
        fs::path child = p / paths::from_utf8(e.name);
        if (e.is_dir) {
            total += size_of(child);
        } else if (e.is_file) {
            auto n = fs::file_size(child, ec);
            if (!ec) total += n;
        }
    }
    return total;
}

// "fmt@12.2.0+s1a2b3c4d" -> "fmt@12.2.0": the name the locks of an entry's libraries use (store.md §2.1, §4).
std::string unsuffixed(const std::string& entry) {
    std::size_t at = entry.rfind("+s");
    return at != std::string::npos && entry.size() - at == 10 ? entry.substr(0, at) : entry;
}

// "fmt@12.2.0+g0123456789ab" -> "fmt@12.2.0"
std::string package_of(const std::string& entry) { return entry.substr(0, entry.find('+')); }

// The URL a git mirror was cloned from, read from its config; the directory name when that fails.
std::string mirror_label(const fs::path& mirror) {
    if (auto config = fsx::try_read_file(mirror / "config")) {
        for (const auto& line : str::split_lines(*config)) {
            std::string_view l = str::trim(line);
            if (l.starts_with("url = ")) return "mirror of " + str::printable(l.substr(6));
        }
    }
    return "cache/git/" + paths::to_utf8(mirror.filename());
}

// File names in store/, cache/downloads/ and cache/git/ that some project uses (store.md §8.2).
struct Used {
    std::set<std::string> entries, archives, mirrors;

    void add(const fs::path& home, const lock::Lockfile& lock) {
        for (const auto& [key, p] : lock.packages) {
            if (std::holds_alternative<lock::PathResolution>(p.resolution)) continue;
            entries.insert(paths::to_utf8(entry_dir(home, p).filename()));
            if (const auto* reg = std::get_if<lock::RegistryResolution>(&p.resolution)) {
                if (reg->integrity.starts_with("sha256:")) archives.insert("sha256-" + reg->integrity.substr(7) + ".tar.gz");
            } else if (const auto* source = std::get_if<lock::GitResolution>(&p.resolution)) {
                mirrors.insert(paths::to_utf8(git::mirror_dir(home, source->url).filename()));
            }
        }
    }
};

bool stale(const fs::path& p) {
    std::error_code ec;
    auto when = fs::last_write_time(p, ec);
    return !ec && fs::file_time_type::clock::now() - when > kStaleTemporary;
}

// Download file names ("sha256-<hex>.tar.gz") mapped to the store entries extracted from them, read from the
// entries' .integrity files.
std::map<std::string, std::string> archive_owners(const fs::path& home) {
    std::map<std::string, std::string> owners;
    for (const auto& e : fsx::list_dir(home / "store")) {
        if (!e.is_dir || e.is_link) continue;
        if (auto integrity = fsx::try_read_file(home / "store" / paths::from_utf8(e.name) / ".integrity")) {
            std::string i(str::trim(*integrity));
            if (i.starts_with("sha256:")) owners.try_emplace("sha256-" + i.substr(7) + ".tar.gz", package_of(e.name));
        }
    }
    return owners;
}

Item archive_item(const fs::path& file, const std::map<std::string, std::string>& owners) {
    std::string name = paths::to_utf8(file.filename());
    auto owner = owners.find(name);
    return {Item::Kind::Archive, file, owner != owners.end() ? "archive of " + owner->second : "cache/downloads/" + name, size_of(file)};
}

// Every child of `dir` as an item of `kind`, labelled "<prefix><name>".
void add_children(std::vector<Item>& items, const fs::path& dir, Item::Kind kind, const std::string& prefix) {
    for (const auto& e : fsx::list_dir(dir)) {
        fs::path p = dir / paths::from_utf8(e.name);
        items.push_back({kind, p, prefix + e.name, size_of(p)});
    }
}

// What `cpm cache clean --all` removes.
std::vector<Item> everything(const fs::path& home) {
    std::vector<Item> items;
    for (const auto& e : fsx::list_dir(home / "store")) {
        if (e.name == ".layout-version") continue;
        fs::path p = home / "store" / paths::from_utf8(e.name);
        items.push_back({e.is_dir && !e.is_link ? Item::Kind::Package : Item::Kind::Other, p, e.name, size_of(p)});
    }
    for (const auto& e : fsx::list_dir(home / "cache")) {
        fs::path p = home / "cache" / paths::from_utf8(e.name);
        if (e.name == "downloads") {
            auto owners = archive_owners(home);
            for (const auto& a : fsx::list_dir(p)) items.push_back(archive_item(p / paths::from_utf8(a.name), owners));
        } else if (e.name == "git") {
            for (const auto& m : fsx::list_dir(p)) {
                fs::path mirror = p / paths::from_utf8(m.name);
                items.push_back({Item::Kind::Mirror, mirror, mirror_label(mirror), size_of(mirror)});
            }
        } else {
            items.push_back({Item::Kind::Other, p, "cache/" + e.name, size_of(p)});
        }
    }
    add_children(items, home / "tmp", Item::Kind::Temporary, "tmp/");
    add_children(items, home / "logs", Item::Kind::Other, "logs/");
    return items;
}

// Deletes an item under the locks that guard it (store.md §4). A store entry or mirror is first renamed into tmp/,
// so it disappears at once rather than file by file. False when another process holds a lock or the files are in use.
bool remove_item(const fs::path& home, const Item& item) {
    std::vector<FileLock> locks;
    auto take = [&](const fs::path& file) {
        auto lock = FileLock::try_acquire(file);
        if (lock) locks.push_back(std::move(*lock));
        return lock.has_value();
    };
    std::string name = paths::to_utf8(item.path.filename());
    if (item.kind == Item::Kind::Package) {
        if (!take(lock_file(home, name))) return false;
        for (const auto& lib : fsx::list_dir(item.path / "lib")) {
            if (lib.is_dir && !take(lock_file(home, std::format("{} for abi {}", unsuffixed(name), lib.name)))) return false;
        }
    } else if (item.kind == Item::Kind::Mirror) {
        if (!take(home / "locks" / ("git-" + paths::to_utf8(item.path.stem()) + ".lock"))) return false;
    }
    try {
        if (item.kind == Item::Kind::Package || item.kind == Item::Kind::Mirror) {
            fs::path trash = make_temp_dir(home, "trash");
            std::error_code ec;
            fs::rename(item.path, trash / item.path.filename(), ec);
            if (ec) {
                fsx::remove_tree(trash);
                return false;
            }
            try {
                fsx::remove_tree(trash);
            } catch (const Error&) {
                // Gone from the store already; what is left in tmp/ goes with the next clean.
            }
        } else {
            fsx::remove_tree(item.path);
        }
    } catch (const Error& e) {
        log::verbose(std::format("could not remove {}: {}", paths::to_utf8(item.path), e.what()));
        return false;
    }
    return true;
}

}  // namespace

std::string project_key(const fs::path& root) {
    std::string key = paths::to_utf8(fs::absolute(root).lexically_normal());
    std::replace(key.begin(), key.end(), '\\', '/');
    while (key.size() > 1 && key.ends_with('/') && !key.ends_with(":/")) key.pop_back();
#ifdef _WIN32
    if (key.size() >= 2 && key[1] == ':' && key[0] >= 'a' && key[0] <= 'z') key[0] = static_cast<char>(key[0] - 'a' + 'A');
#endif
    return key;
}

void register_project(const fs::path& home, const fs::path& root) {
    try {
        std::string key = project_key(root), date = today();
        auto recorded = [&](const std::map<std::string, std::string>& list) {
            auto it = list.find(key);
            return it != list.end() && it->second >= date;
        };
        if (recorded(read_projects(home))) return;
        FileLock lock = lock_projects(home);
        auto list = read_projects(home);
        if (recorded(list)) return;
        list[key] = date;
        write_projects(home, list);
    } catch (const std::exception& e) {
        log::verbose(std::format("could not record this project in {}: {}", paths::to_utf8(projects_file(home)), e.what()));
    }
}

std::map<std::string, std::string> projects(const fs::path& home) { return read_projects(home); }

Survey survey(const fs::path& home) {
    Survey s;
    Used used;
    for (const auto& [root, last] : read_projects(home)) {
        if (!project_exists(root)) {
            s.gone.push_back(root);
            continue;
        }
        s.projects.push_back(root);
        try {
            fs::path dir = paths::from_utf8(root);
            if (auto lock = lock::load(dir, dir)) used.add(home, *lock);
        } catch (const Error& e) {
            s.unreadable.push_back(std::format("{}: {}", root, e.what()));
        }
    }

    fs::path store = home / "store";
    for (const auto& e : fsx::list_dir(store)) {
        if (!e.is_dir || e.is_link) continue;
        fs::path entry = store / paths::from_utf8(e.name);
        std::uintmax_t bytes = size_of(entry);
        ++s.packages;
        s.store_bytes += bytes;
        for (const auto& lib : fsx::list_dir(entry / "lib")) {
            if (lib.is_dir && !lib.is_link) ++s.libraries;
        }
        std::error_code ec;
        bool complete = fs::exists(entry / ".complete", ec);
        if (!complete || !used.entries.contains(e.name)) s.unused.push_back({Item::Kind::Package, entry, e.name, bytes});
    }

    fs::path downloads = home / "cache" / "downloads";
    auto owners = archive_owners(home);
    for (const auto& e : fsx::list_dir(downloads)) {
        if (!e.is_file) continue;
        Item archive = archive_item(downloads / paths::from_utf8(e.name), owners);
        ++s.archives;
        s.download_bytes += archive.bytes;
        if (!used.archives.contains(e.name)) s.unused.push_back(std::move(archive));
    }

    fs::path git = home / "cache" / "git";
    for (const auto& e : fsx::list_dir(git)) {
        if (!e.is_dir || e.is_link) continue;
        fs::path mirror = git / paths::from_utf8(e.name);
        std::uintmax_t bytes = size_of(mirror);
        ++s.mirrors;
        s.git_bytes += bytes;
        if (!used.mirrors.contains(e.name)) s.unused.push_back({Item::Kind::Mirror, mirror, mirror_label(mirror), bytes});
    }

    for (const auto& e : fsx::list_dir(home / "tmp")) {
        fs::path p = home / "tmp" / paths::from_utf8(e.name);
        if (stale(p)) s.unused.push_back({Item::Kind::Temporary, p, "tmp/" + e.name, size_of(p)});
    }
    return s;
}

CleanResult clean(const fs::path& home, bool all, bool dry_run) {
    CleanResult r;
    Survey s = survey(home);
    r.forgotten = s.gone;
    if (!s.gone.empty() && !dry_run) {
        FileLock lock = lock_projects(home);
        auto list = read_projects(home);
        for (const auto& root : s.gone) {
            if (!project_exists(root)) list.erase(root);
        }
        write_projects(home, list);
    }
    std::vector<Item> items = all ? everything(home) : std::move(s.unused);
    for (auto& item : items) {
        if (dry_run || remove_item(home, item)) {
            r.bytes += item.bytes;
            r.removed.push_back(std::move(item));
        } else {
            r.busy.push_back(std::move(item));
        }
    }
    return r;
}

std::string format_bytes(std::uintmax_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    double n = static_cast<double>(bytes) / 1024;
    for (std::string_view unit : {"KB", "MB", "GB"}) {
        if (n < 1024 || unit == "GB") return std::format("{:.1f} {}", n, unit);
        n /= 1024;
    }
    return std::format("{:.1f} TB", n);
}

}  // namespace cpm::install::cache
