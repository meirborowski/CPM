#include "install/fetch.hpp"

#include <algorithm>
#include <format>
#include <fstream>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "install/store.hpp"

namespace cpm::install {

namespace url {

bool is_file(std::string_view u) { return u.starts_with("file://"); }
bool is_http(std::string_view u) { return u.starts_with("https://") || u.starts_with("http://"); }

fs::path to_path(std::string_view u) {
    std::string_view rest = u.substr(7);  // after "file://"
    if (rest.starts_with("localhost/")) rest.remove_prefix(9);
    std::string decoded;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            decoded += static_cast<char>(std::stoi(std::string(rest.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            decoded += rest[i];
        }
    }
    // "/C:/x" is a Windows drive path.
    if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(0, 1);
    return paths::from_utf8(decoded);
}

std::string from_path(const fs::path& p) {
    std::string s = paths::to_utf8(fs::absolute(p).lexically_normal());
    return s.starts_with('/') ? "file://" + s : "file:///" + s;
}

std::string as_base(std::string_view u) {
    std::string s(u);
    while (s.ends_with("//") && !s.ends_with("://")) s.pop_back();
    if (!s.ends_with('/')) s += '/';
    return s;
}

std::string join(std::string_view base, std::string_view ref) {
    if (ref.find("://") != std::string_view::npos) return std::string(ref);
    std::string b = as_base(base);
    if (ref.starts_with('/')) {  // absolute path on the same host
        std::size_t scheme = b.find("://");
        std::size_t host_end = b.find('/', scheme + 3);
        return b.substr(0, host_end) + std::string(ref);
    }
    return b + std::string(ref);
}

}  // namespace url

std::string sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Error(std::format("cannot read '{}'", paths::to_utf8(file)));
    Sha256 h;
    char buf[1 << 16];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) h.update(std::string_view(buf, static_cast<std::size_t>(in.gcount())));
    return Sha256::hex(h.finish());
}

bool valid_integrity(std::string_view i) {
    if (!i.starts_with("sha256:") || i.size() != 7 + 64) return false;
    return std::all_of(i.begin() + 7, i.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool download(const std::string& u, const fs::path& dest, bool allow_http) {
    fsx::create_dirs(dest.parent_path());
    if (url::is_file(u)) {
        fs::path src = url::to_path(u);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) return false;
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) throw Error(std::format("cannot copy {}: {}", u, ec.message()));
        return true;
    }
    if (!url::is_http(u)) throw Error(std::format("unsupported URL '{}'", u), "use https://, or file:// for a local registry");
    if (u.starts_with("http://") && !allow_http) {
        throw Error(std::format("refusing to download {} over plain http", u),
                    "use https, or set \"allowHttp\": true for this registry in $CPM_HOME/config.jsonc");
    }
    auto curl = proc::find_program("curl");
    if (!curl) throw Error("downloading needs curl, which was not found on PATH", "install curl (https://curl.se)");
    proc::Captured r = proc::capture({.argv = {paths::to_utf8(*curl), "-sS", "-L", "--retry", "2", "--proto",
                                               allow_http ? "=https,http" : "=https", "-o", paths::to_utf8(dest), "-w",
                                               "%{http_code}", u}});
    std::string code(str::trim(r.out));
    if (r.exit_code != 0) {
        std::error_code ec;
        fs::remove(dest, ec);
        throw Error(std::format("cannot download {}: {}", u, str::trim(r.err)), "check the URL and your network access");
    }
    if (code == "404" || code == "410") {
        std::error_code ec;
        fs::remove(dest, ec);
        return false;
    }
    if (!code.starts_with('2')) {
        std::error_code ec;
        fs::remove(dest, ec);
        throw Error(std::format("downloading {} failed with HTTP status {}", u, code));
    }
    return true;
}

fs::path cached_archive(const fs::path& home, const std::string& u, const std::string& integrity, bool allow_http,
                        bool offline, const std::string& what) {
    if (!valid_integrity(integrity)) throw Error(std::format("{} has an invalid integrity value \"{}\"", what, integrity));
    std::string expected = integrity.substr(7);
    fs::path cached = home / "cache" / "downloads" / ("sha256-" + expected + ".tar.gz");
    std::error_code ec;
    if (fs::exists(cached, ec)) {
        if (sha256_file(cached) == expected) return cached;
        fs::remove(cached, ec);  // a corrupt cache entry
    }
    if (offline) throw Error(std::format("{} is not in the download cache, and --offline is set", what));

    log::status("Downloading", what);
    fs::path tmp = make_temp_dir(home, "download");
    fs::path file = tmp / "archive";
    try {
        if (!download(u, file, allow_http)) throw Error(std::format("{} does not exist ({})", what, u));
        std::string actual = sha256_file(file);
        if (actual != expected) {
            throw Error(std::format("integrity check failed for {}\n  expected sha256:{}\n  got      sha256:{}", what, expected, actual),
                        "the archive differs from what the registry (or cpm-lock.json) recorded; it was not used");
        }
        fsx::create_dirs(cached.parent_path());
        fsx::rename_replace(file, cached);
    } catch (...) {
        fsx::remove_tree(tmp);
        throw;
    }
    fsx::remove_tree(tmp);
    return cached;
}

namespace {

void reject_links(const fs::path& dir, const std::string& what) {
    for (const auto& e : fsx::list_dir(dir)) {
        if (e.is_link) throw Error(std::format("{} contains a link ({}), which CPM does not accept", what, e.name));
        if (e.is_dir) reject_links(dir / paths::from_utf8(e.name), what);
    }
}

}  // namespace

void extract_archive(const fs::path& archive, const fs::path& dest, int strip, const std::string& what) {
    auto tar = proc::find_program("tar");
    if (!tar) throw Error("extracting packages needs tar, which was not found on PATH");
    fsx::create_dirs(dest);
    std::vector<std::string> argv = {paths::to_utf8(*tar), "-xzf", paths::to_utf8(archive), "-C", paths::to_utf8(dest)};
    if (strip > 0) argv.push_back(std::format("--strip-components={}", strip));
    proc::Captured r = proc::capture({.argv = argv});
    if (r.exit_code != 0) throw Error(std::format("cannot extract {}:\n{}", what, str::trim(r.err)));
    reject_links(dest, what);
}

}  // namespace cpm::install
