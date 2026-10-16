#include "core/env.hpp"

#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/strings.hpp"
#endif

namespace cpm::env {

std::optional<std::string> get(const std::string& name) {
#ifdef _WIN32
    std::wstring wname = str::widen(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(n);
    return str::narrow(value);
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

bool is_set(const std::string& name) {
    auto value = get(name);
    return value && !value->empty();
}

}  // namespace cpm::env
