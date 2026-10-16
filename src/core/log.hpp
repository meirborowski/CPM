#pragma once

#include <string>
#include <string_view>

#include "core/error.hpp"

namespace cpm::log {

enum class Level { Quiet, Normal, Verbose, Trace };

void set_level(Level level);
Level level();
bool verbose();  // level >= Verbose
void set_color(bool enabled);
bool color();

// Styling helpers. They return the text unchanged when colour is off.
std::string bold(std::string_view text);
std::string green(std::string_view text);
std::string red(std::string_view text);
std::string yellow(std::string_view text);
std::string cyan(std::string_view text);
std::string dim(std::string_view text);

// Raw output. Everything CPM says goes to stderr; stdout is reserved for requested data.
void out(std::string_view text);
void err(std::string_view text);

// "   Compiling hello v0.1.0": the verb is right-aligned to 12 columns.
void status(std::string_view verb, std::string_view message);
void info(std::string_view message);
void verbose(std::string_view message);
void trace(std::string_view message);
void warn(std::string_view message, std::string_view hint = {});
void error(std::string_view message, std::string_view hint = {});
void diagnostic(const Diagnostic& d);

}  // namespace cpm::log
