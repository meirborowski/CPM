#include "build/ninja_writer.hpp"

#include <format>

#include "core/proc.hpp"

namespace cpm::build::ninja {

std::string escape_path(std::string_view path) {
    std::string out;
    for (char c : path) {
        if (c == '$' || c == ' ' || c == ':') out += '$';
        out += c;
    }
    return out;
}

std::string escape_value(std::string_view value) {
    std::string out;
    for (char c : value) {
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

std::string args_value(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        if (!out.empty()) out += ' ';
        out += escape_value(proc::quote(a));
    }
    return out;
}

void Writer::comment(std::string_view text) { out_ += std::format("# {}\n", text); }

void Writer::newline() { out_ += '\n'; }

void Writer::variable(std::string_view name, std::string_view raw_value, int indent) {
    out_.append(static_cast<std::size_t>(indent), ' ');
    out_ += name;
    out_ += raw_value.empty() ? " =" : " = ";
    out_ += raw_value;
    out_ += '\n';
}

void Writer::rule(const toolchain::NinjaRule& r) {
    out_ += std::format("rule {}\n", r.name);
    variable("command", r.command, 2);
    if (!r.description.empty()) variable("description", r.description, 2);
    if (!r.depfile.empty()) variable("depfile", r.depfile, 2);
    if (!r.deps.empty()) variable("deps", r.deps, 2);
    if (!r.rspfile.empty()) variable("rspfile", r.rspfile, 2);
    if (!r.rspfile_content.empty()) variable("rspfile_content", r.rspfile_content, 2);
    if (!r.msvc_deps_prefix.empty()) variable("msvc_deps_prefix", escape_value(r.msvc_deps_prefix), 2);
}

void Writer::build(const std::vector<std::string>& outputs, std::string_view rule, const std::vector<std::string>& inputs,
                   const std::vector<std::string>& implicit,
                   const std::vector<std::pair<std::string, std::string>>& variables,
                   const std::vector<std::string>& implicit_outputs) {
    std::string head = "build";
    for (const auto& o : outputs) head += ' ' + escape_path(o);
    if (!implicit_outputs.empty()) {
        head += " |";
        for (const auto& o : implicit_outputs) head += ' ' + escape_path(o);
    }
    head += std::format(": {}", rule);

    std::vector<std::string> tokens;
    for (const auto& i : inputs) tokens.push_back(escape_path(i));
    if (!implicit.empty()) {
        tokens.emplace_back("|");
        for (const auto& i : implicit) tokens.push_back(escape_path(i));
    }

    if (tokens.size() <= 1) {
        out_ += head;
        if (!tokens.empty()) out_ += ' ' + tokens.front();
        out_ += '\n';
    } else {
        // One input per line keeps diffs of generated files readable.
        out_ += head + " $\n";
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            out_ += "    " + tokens[i];
            out_ += i + 1 < tokens.size() ? " $\n" : "\n";
        }
    }
    for (const auto& [name, value] : variables) variable(name, value, 2);
}

void Writer::defaults(const std::vector<std::string>& targets) {
    std::string line = "default";
    for (const auto& t : targets) line += ' ' + escape_path(t);
    out_ += line + '\n';
}

}  // namespace cpm::build::ninja
