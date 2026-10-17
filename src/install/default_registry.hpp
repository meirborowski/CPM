#pragma once

#include <string_view>

namespace cpm::install {

// The registry CPM uses as "default" when neither CPM_REGISTRY nor $CPM_HOME/config.jsonc names one
// (docs/spec/cli.md §5): the public registry service, whose source is github.com/meirborowski/cpm-registry.
inline constexpr std::string_view kBuiltinRegistry = "https://cpm-registry.com/";

}  // namespace cpm::install
