// core/archive: the deterministic .tar.gz writer behind `cpm publish`. Round trips go through the system tar,
// the same reader CPM uses to install packages.
#include <random>

#include "core/archive.hpp"
#include "core/fsx.hpp"
#include "install/fetch.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

TEST(crc32_matches_the_standard_check_value) {
    CHECK_EQ(archive::crc32(""), 0u);
    CHECK_EQ(archive::crc32("123456789"), 0xCBF43926u);
}

TEST(gzip_is_deterministic_and_compresses) {
    std::string text;
    for (int i = 0; i < 2000; ++i) text += std::format("line {} of a fairly repetitive source file\n", i % 50);
    std::string a = archive::gzip(text);
    CHECK_EQ(a, archive::gzip(text));
    CHECK_EQ(a.substr(0, 4), std::string("\x1f\x8b\x08\x00", 4));
    CHECK_EQ(a.substr(4, 4), std::string(4, '\0'));  // mtime 0
    CHECK(a.size() < text.size() / 10);
    CHECK_EQ(archive::gzip("").size(), 20u);  // header, an empty fixed-Huffman block, CRC and size
}

TEST(tar_gz_round_trips_through_the_system_tar) {
    check::TempDir dir;
    std::string binary;
    for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
    std::mt19937 rng(42);
    std::string noise;
    for (int i = 0; i < 100000; ++i) noise.push_back(static_cast<char>(rng() & 0xFF));
    std::string repetitive;
    for (int i = 0; i < 20000; ++i) repetitive += "abcabcabd" + std::to_string(i % 7);
    std::string long_name = "pkg-1.0.0/include/" + std::string(90, 'a') + "/" + std::string(60, 'b') + ".hpp";  // ustar prefix
    // A file name over 100 characters fits no ustar split, so it needs a pax header. (Kept short enough for
    // Windows' 260-character path limit once extracted.)
    std::string longer_name = "pkg-1.0.0/" + std::string(110, 'c') + ".hpp";

    std::vector<archive::Entry> entries = {
        {.path = "pkg-1.0.0/src/b.cpp", .data = "int b() { return 2; }\n"},
        {.path = "pkg-1.0.0/cpm.jsonc", .data = "{ \"name\": \"pkg\" }\n"},
        {.path = "pkg-1.0.0/empty.txt", .data = ""},
        {.path = "pkg-1.0.0/data/binary.bin", .data = binary},
        {.path = "pkg-1.0.0/data/noise.bin", .data = noise},
        {.path = "pkg-1.0.0/data/repetitive.txt", .data = repetitive},
        {.path = long_name, .data = "long\n"},
        {.path = longer_name, .data = "longer\n"},
        {.path = "pkg-1.0.0/docs", .data = {}, .directory = true},
    };
    std::string bytes = archive::gzip(archive::tar(entries));
    CHECK_EQ(bytes, archive::gzip(archive::tar(entries)));
    std::reverse(entries.begin(), entries.end());
    CHECK_EQ(bytes, archive::gzip(archive::tar(entries)));  // input order does not matter

    fs::path file = dir.path() / "pkg.tar.gz";
    fsx::write_file_atomic(file, bytes);
    fs::path out = dir.path() / "out";
    install::extract_archive(file, out, 1, "pkg");
    CHECK_EQ(fsx::read_file(out / "src/b.cpp"), "int b() { return 2; }\n");
    CHECK_EQ(fsx::read_file(out / "empty.txt"), "");
    CHECK_EQ(fsx::read_file(out / "data/binary.bin"), binary);
    CHECK_EQ(fsx::read_file(out / "data/noise.bin"), noise);
    CHECK_EQ(fsx::read_file(out / "data/repetitive.txt"), repetitive);
    CHECK_EQ(fsx::read_file(out / long_name.substr(10)), "long\n");
    CHECK_EQ(fsx::read_file(out / longer_name.substr(10)), "longer\n");
    CHECK(fs::is_directory(out / "docs"));
}

TEST(tar_rejects_unsafe_and_duplicate_paths) {
    for (const char* bad : {"/abs", "a/../b", "a//b", "./a", "a\\b", "C:/x", ""}) {
        CHECK_THROWS(archive::tar({{.path = bad, .data = "x"}}), "invalid member path");
    }
    CHECK_THROWS(archive::tar({{.path = "a", .data = "1"}, {.path = "a", .data = "2"}}), "given twice");
    CHECK_THROWS(archive::tar({{.path = "a", .data = "1"}, {.path = "a/b", .data = "2"}}), "both a file and a directory");
}
