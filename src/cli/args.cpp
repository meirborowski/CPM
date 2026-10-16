#include "cli/args.hpp"

#include <algorithm>
#include <format>

#include "core/error.hpp"

namespace cpm::cli {

bool ParsedArgs::flag(std::string_view long_name) const { return count(long_name) > 0; }

int ParsedArgs::count(std::string_view long_name) const {
    return static_cast<int>(
        std::count_if(options.begin(), options.end(), [&](const auto& o) { return o.first == long_name; }));
}

std::optional<std::string> ParsedArgs::value(std::string_view long_name) const {
    for (auto it = options.rbegin(); it != options.rend(); ++it) {
        if (it->first == long_name) return it->second;
    }
    return std::nullopt;
}

namespace {

const OptionSpec* find_long(std::span<const OptionSpec> a, std::span<const OptionSpec> b, std::string_view name) {
    for (auto specs : {a, b}) {
        for (const auto& o : specs) {
            if (o.long_name == name) return &o;
        }
    }
    return nullptr;
}

const OptionSpec* find_short(std::span<const OptionSpec> a, std::span<const OptionSpec> b, char c) {
    for (auto specs : {a, b}) {
        for (const auto& o : specs) {
            if (o.short_name != 0 && o.short_name == c) return &o;
        }
    }
    return nullptr;
}

const CommandSpec* find_command(std::span<const CommandSpec> commands, std::string_view name) {
    for (const auto& c : commands) {
        if (c.name == name) return &c;
        if (std::find(c.aliases.begin(), c.aliases.end(), name) != c.aliases.end()) return &c;
    }
    return nullptr;
}

std::string where(const ParsedArgs& args) {
    return args.command ? std::format(" for 'cpm {}'", args.command->name) : std::string();
}

}  // namespace

ParsedArgs parse_args(std::span<const std::string> argv, std::span<const OptionSpec> global,
                      std::span<const CommandSpec> commands) {
    ParsedArgs args;
    std::span<const OptionSpec> command_options;

    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& tok = argv[i];

        if (args.has_passthrough) {
            args.passthrough.push_back(tok);
            continue;
        }
        if (tok == "--") {
            args.has_passthrough = true;
            continue;
        }

        if (tok.starts_with("--")) {
            std::string_view body = std::string_view(tok).substr(2);
            std::string_view name = body;
            std::optional<std::string> inline_value;
            if (auto eq = body.find('='); eq != std::string_view::npos) {
                name = body.substr(0, eq);
                inline_value = std::string(body.substr(eq + 1));
            }
            const OptionSpec* spec = find_long(global, command_options, name);
            if (spec == nullptr) {
                throw UsageError(std::format("unknown option '--{}'{}", name, where(args)),
                                 "run 'cpm --help' for the list of commands and options");
            }
            if (spec->takes_value) {
                if (!inline_value) {
                    if (i + 1 >= argv.size()) {
                        throw UsageError(std::format("option '--{}' needs a value {}", name, spec->value_name));
                    }
                    inline_value = argv[++i];
                }
                args.options.emplace_back(std::string(spec->long_name), *inline_value);
            } else {
                if (inline_value) throw UsageError(std::format("option '--{}' does not take a value", name));
                args.options.emplace_back(std::string(spec->long_name), std::string());
            }
            continue;
        }

        if (tok.size() > 1 && tok[0] == '-') {
            for (std::size_t k = 1; k < tok.size(); ++k) {
                const OptionSpec* spec = find_short(global, command_options, tok[k]);
                if (spec == nullptr) {
                    throw UsageError(std::format("unknown option '-{}'{}", tok[k], where(args)),
                                     "run 'cpm --help' for the list of commands and options");
                }
                if (spec->takes_value) {
                    std::string value = tok.substr(k + 1);
                    if (value.empty()) {
                        if (i + 1 >= argv.size()) {
                            throw UsageError(
                                std::format("option '-{}' needs a value {}", tok[k], spec->value_name));
                        }
                        value = argv[++i];
                    }
                    args.options.emplace_back(std::string(spec->long_name), value);
                    break;
                }
                args.options.emplace_back(std::string(spec->long_name), std::string());
            }
            continue;
        }

        if (args.command == nullptr) {
            args.command = find_command(commands, tok);
            if (args.command == nullptr) {
                throw UsageError(std::format("unknown command '{}'", tok),
                                 "run 'cpm --help' for the list of commands");
            }
            command_options = args.command->options;
            continue;
        }
        args.positionals.push_back(tok);
    }
    return args;
}

std::string format_options(std::span<const OptionSpec> options) {
    std::vector<std::pair<std::string, std::string_view>> rows;
    std::size_t width = 0;
    for (const auto& o : options) {
        std::string left = o.short_name ? std::format("-{}, --{}", o.short_name, o.long_name)
                                        : std::format("    --{}", o.long_name);
        if (o.takes_value) left += std::format(" {}", o.value_name);
        width = std::max(width, left.size());
        rows.emplace_back(std::move(left), o.help);
    }
    std::string out;
    for (const auto& [left, help] : rows) out += std::format("  {:<{}}  {}\n", left, width, help);
    return out;
}

}  // namespace cpm::cli
