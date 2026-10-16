#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "toolchain/driver.hpp"

namespace cpm::build::ninja {

// Escapes a path for a `build` line: '$' -> "$$", ' ' -> "$ ", ':' -> "$:".
std::string escape_path(std::string_view path);

// Escapes a variable value: '$' -> "$$".
std::string escape_value(std::string_view value);

// Joins command-line arguments for a Ninja variable: each argument is quoted for the host's command
// interpreter (Ninja passes commands to CreateProcess on Windows and /bin/sh elsewhere), then escaped.
std::string args_value(const std::vector<std::string>& args);

// Emits Ninja syntax deterministically.
class Writer {
public:
    void comment(std::string_view text);
    void newline();
    void variable(std::string_view name, std::string_view raw_value, int indent = 0);  // value already escaped
    void rule(const toolchain::NinjaRule& rule);
    void build(const std::vector<std::string>& outputs, std::string_view rule, const std::vector<std::string>& inputs,
               const std::vector<std::string>& implicit = {},
               const std::vector<std::pair<std::string, std::string>>& variables = {},
               const std::vector<std::string>& implicit_outputs = {});
    void defaults(const std::vector<std::string>& targets);

    const std::string& str() const { return out_; }

private:
    std::string out_;
};

}  // namespace cpm::build::ninja
