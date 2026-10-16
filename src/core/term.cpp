#include "core/term.hpp"

#include <initializer_list>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/strings.hpp"
#else
#include <unistd.h>
#endif

namespace cpm::term {

#ifdef _WIN32
namespace {
HANDLE handle_for(int fd) {
    return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}
}  // namespace

void init() {
    SetConsoleOutputCP(CP_UTF8);
    for (int fd : {1, 2}) {
        HANDLE h = handle_for(fd);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

bool is_terminal(int fd) {
    HANDLE h = handle_for(fd);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode) != 0;
}

std::optional<std::string> read_line() {
    HANDLE h = handle_for(0);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode)) {
        // A console: read UTF-16 so non-ASCII input survives regardless of the code page.
        std::wstring line;
        wchar_t buf[256];
        while (true) {
            DWORD n = 0;
            if (!ReadConsoleW(h, buf, static_cast<DWORD>(std::size(buf)), &n, nullptr) || n == 0) {
                if (line.empty()) return std::nullopt;
                break;
            }
            line.append(buf, n);
            if (line.find(L'\x1a') != std::wstring::npos && line.size() <= 3) return std::nullopt;  // Ctrl-Z
            if (!line.empty() && line.back() == L'\n') break;
        }
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
        return str::narrow(line);
    }
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}
#else
void init() {}

bool is_terminal(int fd) { return isatty(fd) != 0; }

std::optional<std::string> read_line() {
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}
#endif

}  // namespace cpm::term
