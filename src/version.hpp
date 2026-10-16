#pragma once

#include <string_view>

namespace cpm {

// Must match "version" in cpm.jsonc (checked by tests/version.test.cpp).
inline constexpr std::string_view version = "0.1.0";

}  // namespace cpm
