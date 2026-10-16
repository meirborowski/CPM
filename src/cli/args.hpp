#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpm::cli {

struct OptionSpec {
    std::string_view long_name;     // without "--"
    char short_name = 0;            // 0 = none
    bool takes_value = false;
    std::string_view value_name;    // shown in help, e.g. "<name>"
    std::string_view help;
};

struct CommandSpec {
    std::string_view name;
    std::vector<std::string_view> aliases;
    std::string_view summary;
    std::string_view usage;         // e.g. "cpm build [options]"
    std::vector<OptionSpec> options;
};

class ParsedArgs {
public:
    const CommandSpec* command = nullptr;    // nullptr: no command given
    std::vector<std::string> positionals;
    std::vector<std::string> passthrough;    // everything after "--"
    bool has_passthrough = false;
    std::vector<std::pair<std::string, std::string>> options;  // (long name, value) in order

    bool flag(std::string_view long_name) const;
    int count(std::string_view long_name) const;
    std::optional<std::string> value(std::string_view long_name) const;  // last occurrence
};

// Parses argv (without the program name). Global options may appear before or after the command.
// Throws UsageError for unknown commands or options and for missing option values.
ParsedArgs parse_args(std::span<const std::string> argv, std::span<const OptionSpec> global,
                      std::span<const CommandSpec> commands);

// Help text for an option table, one option per line.
std::string format_options(std::span<const OptionSpec> options);

}  // namespace cpm::cli
