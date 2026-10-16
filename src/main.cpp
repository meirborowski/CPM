#include <string>
#include <vector>

#include "cli/commands.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "core/strings.hpp"
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // argv is in the ANSI code page on Windows; re-read the command line as UTF-16.
    (void)argc;
    (void)argv;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; wargv != nullptr && i < wargc; ++i) args.push_back(cpm::str::narrow(wargv[i]));
    if (wargv != nullptr) LocalFree(wargv);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return cpm::cli::run(args);
}
