#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "json/value.hpp"

namespace cpm::json {

struct ParseOptions {
    bool comments = true;         // allow // and /* */ (JSONC)
    bool trailing_commas = true;  // allow [1, 2,] and {"a": 1,}
    std::size_t max_depth = 256;
};

// Strict JSON: no comments, no trailing commas.
inline constexpr ParseOptions kStrict{false, false, 256};

class ParseError : public Error {
public:
    ParseError(std::string message, std::uint32_t offset) : Error(std::move(message)), offset_(offset) {}
    std::uint32_t offset() const noexcept { return offset_; }

private:
    std::uint32_t offset_;
};

// Parses a JSON or JSONC document. Duplicate object keys are an error. Throws ParseError.
Value parse(std::string_view text, const ParseOptions& options = {});

// Turns a span into a displayable location (line, column, the source line).
SourceLoc locate(std::string_view text, Span span, std::string file);

}  // namespace cpm::json
