#include "core/system_tar.hpp"

#include <format>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"

namespace cpm::system_tar {

namespace {

// locate()'s tar, probed once.
const std::optional<Tar>& found() {
    static const std::optional<Tar> tar = []() -> std::optional<Tar> {
        auto program = locate();
        if (!program) return std::nullopt;
        return probe(*program);
    }();
    return tar;
}

}  // namespace

std::optional<fs::path> locate() {
#ifdef _WIN32
    if (auto root = env::get("SystemRoot")) {
        if (auto windows_tar = proc::find_program(*root + "\\System32\\tar.exe")) return windows_tar;
    }
#endif
    return proc::find_program("tar");
}

bool is_gnu(std::string_view version_output) { return version_output.find("GNU tar") != std::string_view::npos; }

Tar probe(const fs::path& program) {
    proc::Captured r = proc::capture({.argv = {paths::to_utf8(program), "--version"}});
    return {.program = program, .gnu = is_gnu(r.out)};
}

std::vector<std::string> extract_argv(const Tar& tar, const Extraction& extraction) {
    std::vector<std::string> argv = {paths::to_utf8(tar.program)};
    if (tar.gnu) argv.push_back("--force-local");
    argv.insert(argv.end(), {extraction.gzip ? "-xzf" : "-xf", paths::to_utf8(extraction.archive), "-C",
                             paths::to_utf8(extraction.dest)});
    if (extraction.strip > 0) argv.push_back(std::format("--strip-components={}", extraction.strip));
    return argv;
}

void extract(const Extraction& extraction, std::string_view what) {
    const std::optional<Tar>& tar = found();
    if (!tar) throw Error(std::format("extracting {} needs tar, which was not found on PATH", what));
    fsx::create_dirs(extraction.dest);
    proc::Captured r = proc::capture({.argv = extract_argv(*tar, extraction)});
    if (r.exit_code != 0) throw Error(std::format("cannot extract {}:\n{}", what, str::trim(r.err)));
}

}  // namespace cpm::system_tar
