#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "json/value.hpp"

namespace cpm::manifest {

// Collects located diagnostics while reading a JSON(C) document, with typed accessors that report
// type mismatches instead of throwing.
class Checker {
public:
    Checker(std::string_view text, std::string display_file) : text_(text), file_(std::move(display_file)) {}

    void error(json::Span span, std::string message, std::string hint = {});
    void warning(json::Span span, std::string message, std::string hint = {});

    // Typed member access. A missing key yields nullopt silently; a wrong type is reported.
    std::optional<std::string> string(const json::Value& obj, std::string_view key);
    std::optional<bool> boolean(const json::Value& obj, std::string_view key);
    std::optional<std::vector<std::string>> strings(const json::Value& obj, std::string_view key);
    const json::Value* object(const json::Value& obj, std::string_view key);

    // Reports a type mismatch for `v` (used for values that are not plain members).
    void type_error(const json::Value& v, std::string_view what, std::string_view expected);

    // Warns about members not in `known` (with a "did you mean" hint). Keys starting with '$' are
    // reported only if they are not "$schema".
    void unknown_keys(const json::Value& obj, std::span<const std::string_view> known, std::string_view where);

    bool has_errors() const { return !errors_.empty(); }
    // All diagnostics (errors and warnings) sorted by position.
    std::vector<Diagnostic> take_all();
    std::vector<Diagnostic> take_warnings();

    const std::string& file() const { return file_; }

private:
    Diagnostic make(Severity sev, json::Span span, std::string message, std::string hint) const;

    std::string_view text_;
    std::string file_;
    std::vector<std::pair<std::uint32_t, Diagnostic>> errors_;
    std::vector<std::pair<std::uint32_t, Diagnostic>> warnings_;
};

// The closest candidate within edit distance 2, or empty.
std::string closest_match(std::string_view word, std::span<const std::string_view> candidates);

}  // namespace cpm::manifest
