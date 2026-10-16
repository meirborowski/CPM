#include "core/paths.hpp"

#include "core/env.hpp"
#include "core/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <vector>
#endif

namespace cpm::paths {

std::string to_utf8(const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string to_native_utf8(const fs::path& p) {
    fs::path n = p.lexically_normal();
    n.make_preferred();
    std::u8string s = n.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path home_dir() {
#ifdef _WIN32
    if (auto v = env::get("USERPROFILE"); v && !v->empty()) return from_utf8(*v);
#else
    if (auto v = env::get("HOME"); v && !v->empty()) return from_utf8(*v);
#endif
    throw Error("cannot determine the home directory", "set CPM_HOME to choose where CPM keeps its data");
}

fs::path cpm_home() {
    if (auto v = env::get("CPM_HOME"); v && !v->empty()) return fs::absolute(from_utf8(*v)).lexically_normal();
    return home_dir() / ".cpm";
}

fs::path current_exe() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) throw Error("cannot determine the path of the running executable");
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) throw Error("cannot determine the path of the running executable");
    return fs::weakly_canonical(fs::path(buf.data()));
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) throw Error("cannot determine the path of the running executable");
    return p;
#endif
}

std::optional<fs::path> find_project_root(const fs::path& start) {
    fs::path dir = fs::absolute(start).lexically_normal();
    while (true) {
        std::error_code ec;
        if (fs::is_regular_file(dir / "cpm.jsonc", ec)) return dir;
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) return std::nullopt;
        dir = parent;
    }
}

std::string display(const fs::path& p, const fs::path& base) {
    fs::path rel = p.lexically_relative(base);
    if (!rel.empty() && *rel.begin() != "..") return to_utf8(rel);
    return to_utf8(p);
}

std::string relative_or_absolute(const fs::path& p, const fs::path& base) {
    if (p.is_relative()) return to_utf8(p);
    if (p.root_name() != base.root_name()) return to_utf8(p);
    fs::path rel = p.lexically_relative(base);
    if (rel.empty()) return to_utf8(p);
    return to_utf8(rel);
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    bool same = fs::equivalent(a, b, ec);
    return !ec && same;
}

}  // namespace cpm::paths
