#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Extracting archives with the system's tar program (docs/spec/architecture.md §3.1). CPM writes its own
// archives (core/archive), and reads them with this.
namespace cpm::system_tar {

namespace fs = std::filesystem;

// A tar program, and whether it is GNU tar.
struct Tar {
    fs::path program;
    bool gnu = false;
};

// What to extract, and where.
struct Extraction {
    fs::path archive;
    fs::path dest;
    bool gzip = false;  // a .tar.gz
    int strip = 0;      // leading path components to drop, as with tar --strip-components
};

// The tar CPM extracts with. On Windows, %SystemRoot%\System32\tar.exe (bsdtar, part of Windows since 10 version
// 1803) when it exists: in Git Bash or MSYS2 the first tar on PATH is GNU tar. Otherwise the first tar on PATH,
// or nullopt if there is none.
std::optional<fs::path> locate();

// Whether `tar --version` printed this as GNU tar ("tar (GNU tar) 1.35"), rather than bsdtar ("bsdtar 3.7.2 -
// libarchive 3.7.2 ...") or BusyBox, which prints its usage.
bool is_gnu(std::string_view version_output);

// Asks a tar program which tar it is, with `tar --version`.
Tar probe(const fs::path& program);

// The command line that extracts with `tar`. GNU tar gets --force-local: it reads an archive path whose first ':'
// comes before any '/', such as C:/x.tar.gz, as host:path on another machine. Other tars reject the option.
std::vector<std::string> extract_argv(const Tar& tar, const Extraction& extraction);

// Extracts into `extraction.dest`, which it creates, with the tar from locate() (probed once per process).
// Throws cpm::Error naming `what` when there is no tar, or with tar's messages when extracting fails.
void extract(const Extraction& extraction, std::string_view what);

}  // namespace cpm::system_tar
