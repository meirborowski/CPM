#pragma once

#include <optional>
#include <string>
#include <vector>

#include "manifest/manifest.hpp"

namespace cpm::build {

// What the directory layout says about a package (docs/spec/manifest.md §4-5).
struct Layout {
    manifest::TargetKind kind = manifest::TargetKind::Executable;
    std::optional<std::string> entry;   // root-relative; the executable entrypoint, or src/lib.* for libraries
    std::vector<std::string> sources;   // translation units besides the executable entrypoint and tests
    std::vector<std::string> tests;     // tests/*.test.{cpp,cc,cxx,c}
    bool include_dir_exists = false;
    std::vector<std::string> notes;     // things worth a warning
};

// Throws cpm::Error when the type cannot be inferred or contradicts the files on disk.
Layout infer_layout(const manifest::Manifest& m, const manifest::PlatformOverrides& platform);

}  // namespace cpm::build
