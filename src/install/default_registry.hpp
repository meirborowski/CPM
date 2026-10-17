#pragma once

#include <string_view>

namespace cpm::install {

// The registry CPM uses as "default" when neither CPM_REGISTRY nor $CPM_HOME/config.jsonc names one
// (docs/spec/cli.md §5). A static registry's base URL, e.g. "https://example.org/cpm-registry/".
// Empty: this build has no built-in registry, so registry dependencies need one configured.
inline constexpr std::string_view kBuiltinRegistry = "";

}  // namespace cpm::install
