#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cpm {

// A location in a source file, ready for display.
struct SourceLoc {
    std::string file;          // path as shown to the user
    std::uint32_t line = 0;    // 1-based
    std::uint32_t col = 0;     // 1-based, in code points
    std::uint32_t length = 1;  // caret length, in code points
    std::string line_text;     // the whole source line, without the newline
};

enum class Severity { Error, Warning };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string message;
    std::string hint;
    std::optional<SourceLoc> loc;
};

// A fatal, user-facing error. main() prints it and exits with code 1.
class Error : public std::runtime_error {
public:
    explicit Error(std::string message, std::string hint = {})
        : std::runtime_error(std::move(message)), hint_(std::move(hint)) {}

    const std::string& hint() const noexcept { return hint_; }

private:
    std::string hint_;
};

// Invalid command-line usage. main() exits with code 2.
class UsageError : public Error {
public:
    using Error::Error;
};

// One or more located problems (for example, manifest validation errors).
class DiagnosticsError : public Error {
public:
    explicit DiagnosticsError(std::vector<Diagnostic> diagnostics)
        : Error(diagnostics.empty() ? std::string("invalid input") : diagnostics.front().message),
          diagnostics_(std::move(diagnostics)) {}

    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace cpm
