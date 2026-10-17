#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Writing .tar.gz archives without any third-party code, for `cpm publish` (docs/spec/registry.md §6).
// Reading them stays with the system `tar`.
namespace cpm::archive {

// One member of a tar archive. `path` is relative and '/'-separated, without a trailing '/'.
struct Entry {
    std::string path;
    std::string data;  // the file's contents; empty for a directory
    bool directory = false;
};

// A POSIX tar archive, written deterministically: members sorted by path (parents before their contents),
// missing parent directories added, mtime 0, uid and gid 0, no user or group names, mode 0755 for
// directories and 0644 for files. Paths that do not fit a ustar header get a pax header. Throws cpm::Error
// for an absolute path, an empty segment, "." or "..", or the same path given twice.
std::string tar(std::vector<Entry> entries);

// gzip (RFC 1952) around DEFLATE data (RFC 1951, LZ77 with fixed Huffman codes). No file name, mtime 0 and
// OS "unknown", so the same input always gives the same bytes on every platform.
std::string gzip(std::string_view data);

// The CRC-32 used by gzip and zip.
std::uint32_t crc32(std::string_view data);

}  // namespace cpm::archive
