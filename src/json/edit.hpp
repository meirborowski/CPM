#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "json/value.hpp"

// Surgical edits of JSONC text (docs/spec/manifest.md §10): only the edited member changes; every other
// byte, comments included, stays as it was.
namespace cpm::json::edit {

// Sets the member at `path` (a chain of object keys) to `value`, creating missing objects. A new member
// goes at its sorted position when the existing keys are sorted, otherwise at the end. Indentation and
// line endings are copied from the surrounding text. Throws cpm::Error if a path element is not an object.
std::string set_member(std::string_view text, const std::vector<std::string>& path, const Value& value);

// Removes the member at `path` together with its separating comma and any comment on the same line.
// Returns the text unchanged if the member does not exist.
std::string remove_member(std::string_view text, const std::vector<std::string>& path);

}  // namespace cpm::json::edit
