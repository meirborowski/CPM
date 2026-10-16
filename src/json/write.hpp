#pragma once

#include <string>
#include <string_view>

#include "json/value.hpp"

namespace cpm::json {

struct WriteOptions {
    int indent = 2;          // spaces per level; ignored when compact
    bool compact = false;    // {"a":1,"b":[2]}
    bool sort_keys = false;  // byte-wise order of UTF-8 keys, at every level
};

// Serialises a value deterministically (no trailing newline). Formatting follows docs/spec/lockfile.md §3:
// one member per line, `": "` separators, empty containers as {} / [], minimal string escaping.
std::string write(const Value& value, const WriteOptions& options = {});

// A JSON string literal, including the quotes.
std::string quote(std::string_view text);

}  // namespace cpm::json
