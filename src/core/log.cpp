#include "core/log.hpp"

#include <cstdio>
#include <format>

#include "core/strings.hpp"

namespace cpm::log {

namespace {
Level g_level = Level::Normal;
bool g_color = false;

std::string paint(std::string_view code, std::string_view text) {
    if (!g_color) return std::string(text);
    return std::format("\x1b[{}m{}\x1b[0m", code, text);
}

void write(std::FILE* f, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
}
}  // namespace

void set_level(Level level) { g_level = level; }
Level level() { return g_level; }
bool verbose() { return g_level >= Level::Verbose; }
void set_color(bool enabled) { g_color = enabled; }
bool color() { return g_color; }

std::string bold(std::string_view text) { return paint("1", text); }
std::string green(std::string_view text) { return paint("1;32", text); }
std::string red(std::string_view text) { return paint("1;31", text); }
std::string yellow(std::string_view text) { return paint("1;33", text); }
std::string cyan(std::string_view text) { return paint("1;36", text); }
std::string dim(std::string_view text) { return paint("2", text); }

void out(std::string_view text) { write(stdout, text); }
void err(std::string_view text) { write(stderr, text); }

void status(std::string_view verb, std::string_view message) {
    if (g_level == Level::Quiet) return;
    err(std::format("{} {}\n", green(std::format("{:>12}", verb)), message));
}

void info(std::string_view message) {
    if (g_level == Level::Quiet) return;
    err(std::format("{}\n", message));
}

void verbose(std::string_view message) {
    if (g_level < Level::Verbose) return;
    err(std::format("{}\n", dim(message)));
}

void trace(std::string_view message) {
    if (g_level < Level::Trace) return;
    err(std::format("{} {}\n", dim("trace:"), dim(message)));
}

void warn(std::string_view message, std::string_view hint) {
    diagnostic(Diagnostic{Severity::Warning, std::string(message), std::string(hint), std::nullopt});
}

void error(std::string_view message, std::string_view hint) {
    diagnostic(Diagnostic{Severity::Error, std::string(message), std::string(hint), std::nullopt});
}

void diagnostic(const Diagnostic& d) {
    bool is_error = d.severity == Severity::Error;
    if (!is_error && g_level == Level::Quiet) return;

    std::string text = is_error ? red("error:") : yellow("warning:");
    text += ' ';
    if (d.loc) text += bold(std::format("{}:{}:{}:", d.loc->file, d.loc->line, d.loc->col)) + ' ';
    text += d.message;
    text += '\n';

    if (d.loc && !d.loc->line_text.empty()) {
        std::string line = str::replace_all(d.loc->line_text, "\t", " ");
        std::string number = std::to_string(d.loc->line);
        std::string gutter(std::max<std::size_t>(number.size(), 4), ' ');
        text += std::format("{:>{}} {} {}\n", number, gutter.size() + 1, dim("|"), line);
        std::size_t col0 = d.loc->col > 0 ? d.loc->col - 1 : 0;
        std::size_t caret_len = std::max<std::size_t>(d.loc->length, 1);
        std::size_t line_len = str::utf8_length(line);
        caret_len = col0 >= line_len ? 1 : std::min(caret_len, line_len - col0);
        std::string pad(col0, ' ');
        std::string carets(caret_len, '^');
        text += std::format("{} {} {}{}\n", std::string(gutter.size() + 1, ' '), dim("|"), pad,
                            is_error ? red(carets) : yellow(carets));
    }
    if (!d.hint.empty()) text += std::format("  {} {}\n", cyan("hint:"), d.hint);
    err(text);
}

}  // namespace cpm::log
