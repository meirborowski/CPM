#include "core/fsx.hpp"

#include <atomic>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cpm::fsx {

namespace {

std::string show(const fs::path& p) { return paths::to_utf8(p); }

#ifdef _WIN32
std::string last_error_text() {
    DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring text = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.')) text.pop_back();
    std::string out;
    for (wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
    return std::format("{} (code {})", out, code);
}
#endif

}  // namespace

std::optional<std::string> try_read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return std::move(ss).str();
}

std::string read_file(const fs::path& p) {
    auto content = try_read_file(p);
    if (!content) throw Error(std::format("cannot read '{}'", show(p)));
    return std::move(*content);
}

void create_dirs(const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p)) throw Error(std::format("cannot create directory '{}': {}", show(p), ec.message()));
}

void rename_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    // Virus scanners and the search indexer briefly open new files without FILE_SHARE_DELETE, which makes the
    // rename fail with "access denied" or a sharing violation. Retry for about half a second, as npm and Cargo do.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        DWORD code = GetLastError();
        bool transient = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (!transient || attempt == 10) {
            SetLastError(code);
            throw Error(std::format("cannot rename '{}' to '{}': {}", show(from), show(to), last_error_text()));
        }
        Sleep(static_cast<DWORD>(10 * (attempt + 1)));  // 10 ms, 20 ms, ... 550 ms in all
    }
#else
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throw Error(std::format("cannot rename '{}' to '{}': {}", show(from), show(to), std::strerror(errno)));
    }
#endif
}

void write_file_atomic(const fs::path& p, std::string_view content) {
    static std::atomic<unsigned> counter{0};
    create_dirs(p.parent_path());
    fs::path tmp = p;
#ifdef _WIN32
    tmp += std::format(".tmp-{}-{}", GetCurrentProcessId(), counter++);
#else
    tmp += std::format(".tmp-{}-{}", static_cast<long>(getpid()), counter++);
#endif
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error(std::format("cannot write '{}'", show(tmp)));
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw Error(std::format("cannot write '{}'", show(tmp)));
        }
    }
    try {
        rename_replace(tmp, p);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

bool write_if_changed(const fs::path& p, std::string_view content) {
    if (auto existing = try_read_file(p); existing && *existing == content) return false;
    write_file_atomic(p, content);
    return true;
}

bool is_link(const fs::path& p) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

namespace {

bool is_protected(const fs::path& p) {
    fs::path abs = fs::absolute(p).lexically_normal();
    if (abs.relative_path().empty() || abs.relative_path() == ".") return true;  // a filesystem root
    if (!abs.has_filename()) abs = abs.parent_path();                         // "dir/" -> "dir"
    auto same = [&](const fs::path& other) {
        try {
            return abs == fs::absolute(other).lexically_normal();
        } catch (...) {
            return false;
        }
    };
    try {
        if (same(paths::home_dir())) return true;
    } catch (const Error&) {
    }
    try {
        fs::path home = paths::cpm_home();
        if (same(home) || same(home / "store")) return true;
    } catch (const Error&) {
    }
    return false;
}

void remove_entry(const fs::path& p) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), attrs & ~DWORD{FILE_ATTRIBUTE_READONLY});
    bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        // A directory junction or directory symlink is removed with RemoveDirectoryW, which deletes the
        // link itself and never its target.
        BOOL ok = is_dir ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str());
        if (!ok) throw Error(std::format("cannot remove link '{}': {}", show(p), last_error_text()));
        return;
    }
    if (is_dir) {
        for (const auto& entry : fs::directory_iterator(p)) remove_entry(entry.path());
        if (!RemoveDirectoryW(p.c_str())) throw Error(std::format("cannot remove '{}': {}", show(p), last_error_text()));
    } else if (!DeleteFileW(p.c_str())) {
        throw Error(std::format("cannot remove '{}': {}", show(p), last_error_text()));
    }
#else
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        for (const auto& entry : fs::directory_iterator(p)) remove_entry(entry.path());
        if (rmdir(p.c_str()) != 0) throw Error(std::format("cannot remove '{}': {}", show(p), std::strerror(errno)));
    } else if (unlink(p.c_str()) != 0) {
        throw Error(std::format("cannot remove '{}': {}", show(p), std::strerror(errno)));
    }
#endif
}

}  // namespace

std::vector<DirEntry> list_dir(const fs::path& dir) {
    std::vector<DirEntry> out;
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    fs::path pattern = dir / L"*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        DirEntry e;
        e.name = str::narrow(name);
        e.is_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.is_dir = !e.is_link && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_file = !e.is_link && !e.is_dir;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &data));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return out;
    while (dirent* ent = readdir(d)) {
        std::string_view name = ent->d_name;
        if (name == "." || name == "..") continue;
        DirEntry e;
        e.name = std::string(name);
        unsigned char type = ent->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (lstat((dir / e.name).c_str(), &st) == 0) {
                type = S_ISLNK(st.st_mode) ? DT_LNK : S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
            }
        }
        e.is_link = type == DT_LNK;
        e.is_dir = type == DT_DIR;
        e.is_file = type == DT_REG;
        out.push_back(std::move(e));
    }
    closedir(d);
#endif
    return out;
}

std::string_view to_string(LinkKind kind) {
    switch (kind) {
        case LinkKind::Symlink: return "symlink";
        case LinkKind::Junction: return "junction";
        case LinkKind::Copy: return "copy";
    }
    return "?";
}

namespace {

#ifdef _WIN32
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

bool make_symlink(const fs::path& target, const fs::path& link) {
    return CreateSymbolicLinkW(link.c_str(), target.c_str(),
                               SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
}

// A directory junction (mount point reparse point). Works without privileges, on local volumes.
bool make_junction(const fs::path& target, const fs::path& link) {
    std::wstring print = target.native();
    while (print.size() > 3 && (print.back() == L'\\' || print.back() == L'/')) print.pop_back();
    std::wstring substitute = L"\\??\\" + print;  // NT path prefix
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    const std::size_t sub_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print.size() * sizeof(wchar_t);
    const std::size_t data_len = 8 + sub_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
    std::vector<unsigned char> buf(8 + data_len, 0);
    auto put16 = [&](std::size_t at, std::size_t v) {
        std::uint16_t x = static_cast<std::uint16_t>(v);
        std::memcpy(buf.data() + at, &x, 2);
    };
    std::uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buf.data(), &tag, 4);
    put16(4, data_len);                       // ReparseDataLength
    put16(8, 0);                              // SubstituteNameOffset
    put16(10, sub_bytes);                     // SubstituteNameLength
    put16(12, sub_bytes + sizeof(wchar_t));   // PrintNameOffset
    put16(14, print_bytes);                   // PrintNameLength
    std::memcpy(buf.data() + 16, substitute.data(), sub_bytes);
    std::memcpy(buf.data() + 16 + sub_bytes + sizeof(wchar_t), print.data(), print_bytes);
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                              &returned, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok != 0;
}
#else
bool make_symlink(const fs::path& target, const fs::path& link) { return symlink(target.c_str(), link.c_str()) == 0; }
#endif

}  // namespace

LinkKind link_directory(const fs::path& target_in, const fs::path& link, std::optional<LinkKind> only) {
    fs::path target = fs::absolute(target_in).lexically_normal();
    target.make_preferred();
    std::error_code ec;
    if (!fs::is_directory(target, ec)) throw Error(std::format("cannot link to '{}': not a directory", show(target)));
    create_dirs(link.parent_path());

    if ((!only || *only == LinkKind::Symlink) && make_symlink(target, link)) return LinkKind::Symlink;
#ifdef _WIN32
    if ((!only || *only == LinkKind::Junction) && make_junction(target, link)) return LinkKind::Junction;
#endif
    if (!only || *only == LinkKind::Copy) {
        fs::copy(target, link, fs::copy_options::recursive, ec);
        if (!ec) return LinkKind::Copy;
    }
    throw Error(std::format("cannot link '{}' to '{}'", show(link), show(target)),
                "check permissions, or set \"linkMode\": \"copy\" in $CPM_HOME/config.jsonc");
}

void remove_tree(const fs::path& p) {
    if (p.empty()) return;
    if (is_protected(p)) throw Error(std::format("refusing to delete '{}'", show(p)));
    remove_entry(p);
}

}  // namespace cpm::fsx
