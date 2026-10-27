#include "toolchain/driver.hpp"

#include <format>

#include "core/error.hpp"

namespace cpm::toolchain {

std::unique_ptr<Driver> make_driver(const Toolchain& tc) {
    switch (tc.family) {
        case Family::Gcc:
        case Family::Clang:
        case Family::AppleClang: return make_gcc_driver(tc);
        case Family::Msvc: return make_msvc_driver(tc);
        case Family::ClangCl: break;
    }
    throw Error(std::format("CPM has no driver for {}", to_string(tc.family)), "use --toolchain msvc, gcc or clang");
}

}  // namespace cpm::toolchain
