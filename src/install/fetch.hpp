#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// URLs, downloads and archive integrity (docs/spec/registry.md §4-5, store.md §3).
namespace cpm::install {

namespace fs = std::filesystem;

namespace url {
bool is_file(std::string_view u);                        // file://...
bool is_http(std::string_view u);                        // http:// or https://
fs::path to_path(std::string_view file_url);             // file:///C:/x -> C:/x, file:///srv/x -> /srv/x
std::string from_path(const fs::path& p);                // the file:// URL of an absolute path
std::string as_base(std::string_view u);                 // with exactly one trailing '/'
std::string join(std::string_view base, std::string_view ref);  // ref if absolute, else base + ref (RFC 3986 subset)
std::string encode(std::string_view text);               // percent-encodes all but RFC 3986 unreserved characters
}  // namespace url

// Lowercase hex SHA-256 of a file's bytes.
std::string sha256_file(const fs::path& file);

// Checks "sha256:<64 hex>" syntax.
bool valid_integrity(std::string_view integrity);

// Downloads `u` to `dest`: file:// by copying, https:// (and http:// when allowed) with curl.
// Returns false on HTTP 404 / missing file; throws on other failures.
bool download(const std::string& u, const fs::path& dest, bool allow_http);

// The archive with this integrity from $CPM_HOME/cache/downloads (content-addressed), downloading and
// verifying it when missing. Throws on an integrity mismatch, or when offline and not cached.
fs::path cached_archive(const fs::path& home, const std::string& u, const std::string& integrity, bool allow_http,
                        bool offline, const std::string& what);

// Extracts a .tar.gz into `dest` (created), dropping `strip` leading path components, and rejects
// archives that contain links (registry.md §4.1).
void extract_archive(const fs::path& archive, const fs::path& dest, int strip, const std::string& what);

}  // namespace cpm::install
