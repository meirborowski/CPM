#pragma once

#include <filesystem>
#include <optional>

#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

// $CPM_HOME/toolchains.json (docs/spec/build.md §1.3): what probing each compiler found, keyed by the C++
// compiler's path. An entry is reused while the compiler file's size and mtime stay the same, and so is its C
// compiler while that file's do.

// The probed toolchain for `cxx`, or nullopt when it has to be probed: no entry, or the compiler changed. The C
// compiler is left out when its file changed, so that it is probed again when a build needs it.
std::optional<Toolchain> cached_toolchain(const std::filesystem::path& cxx, const std::filesystem::path& home);

// Records `tc`, with its C compiler when it has one. Failing to write the cache is not an error.
void cache_toolchain(const Toolchain& tc, const std::filesystem::path& home);

}  // namespace cpm::toolchain
