#pragma once

#include <optional>
#include <string>

namespace cpm::env {

// Value of an environment variable as UTF-8, or nullopt if unset.
std::optional<std::string> get(const std::string& name);

// True if the variable is set to a non-empty value.
bool is_set(const std::string& name);

}  // namespace cpm::env
