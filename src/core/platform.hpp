#pragma once

#include <string_view>

namespace cpm {

enum class Os { Windows, Linux, MacOS, FreeBSD };

constexpr Os host_os() {
#if defined(_WIN32)
    return Os::Windows;
#elif defined(__APPLE__)
    return Os::MacOS;
#elif defined(__FreeBSD__)
    return Os::FreeBSD;
#else
    return Os::Linux;
#endif
}

constexpr std::string_view to_string(Os os) {
    switch (os) {
        case Os::Windows: return "windows";
        case Os::Linux: return "linux";
        case Os::MacOS: return "macos";
        case Os::FreeBSD: return "freebsd";
    }
    return "unknown";
}

constexpr bool is_unix(Os os) { return os != Os::Windows; }

}  // namespace cpm
