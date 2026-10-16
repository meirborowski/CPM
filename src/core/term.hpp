#pragma once

#include <optional>
#include <string>

namespace cpm::term {

// Prepares the console: UTF-8 output and ANSI escape processing on Windows. Idempotent.
void init();

// True if the given standard stream (0 = stdin, 1 = stdout, 2 = stderr) is an interactive terminal.
bool is_terminal(int fd);

// One line from stdin as UTF-8, without the line ending; nullopt at end of input.
std::optional<std::string> read_line();

}  // namespace cpm::term
