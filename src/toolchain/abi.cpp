#include "toolchain/abi.hpp"

#include <algorithm>
#include <array>

#include "core/error.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"

namespace cpm::toolchain {

bool is_abi_define(std::string_view define) {
    static constexpr std::array<std::string_view, 11> names = {
        "_GLIBCXX_DEBUG",      "_GLIBCXX_DEBUG_PEDANTIC", "_GLIBCXX_USE_CXX11_ABI",  "_GLIBCXX_FULLY_DYNAMIC_STRING",
        "_LIBCPP_ABI_VERSION", "_LIBCPP_ABI_UNSTABLE",    "_LIBCPP_HARDENING_MODE",  "_ITERATOR_DEBUG_LEVEL",
        "_HAS_ITERATOR_DEBUGGING", "_SECURE_SCL",         "_HAS_EXCEPTIONS"};
    std::string_view name = define.substr(0, define.find('='));
    return name.starts_with("_LIBCPP_ABI_") || std::find(names.begin(), names.end(), name) != names.end();
}

std::string AbiFingerprint::canonical() const {
    std::string out;
    for (const auto& [k, v] : inputs) out += k + "=" + v + "\n";
    return out;
}

AbiFingerprint fingerprint(const Toolchain& tc, const Driver& driver, const manifest::Profile& profile,
                           const std::string& std, const std::vector<std::string>& defines,
                           const std::vector<std::string>& cxxflags) {
    std::vector<std::string> sanitizers = driver.sanitizers(profile);
    std::sort(sanitizers.begin(), sanitizers.end());

    std::vector<std::string> abi_defines;
    for (const auto& d : defines) {
        if (is_abi_define(d) && std::find(abi_defines.begin(), abi_defines.end(), d) == abi_defines.end())
            abi_defines.push_back(d);
    }
    std::sort(abi_defines.begin(), abi_defines.end());

    auto has_flag = [&](std::string_view flag) { return std::find(cxxflags.begin(), cxxflags.end(), flag) != cxxflags.end(); };

    AbiFingerprint fp;
    auto add = [&](std::string key, std::string value) {
        if (value.find('\n') != std::string::npos) throw Error("internal error: ABI input with a newline");
        fp.inputs.emplace_back(std::move(key), std::move(value));
    };
    add("abi.scheme", "2");
    add("os", std::string(to_string(tc.os)));
    add("arch", tc.arch);
    add("target", tc.target);
    add("compiler", std::string(to_string(tc.family)));
    add("compiler.version", tc.version);
    add("stdlib", tc.stdlib);
    add("stdlib.abi", tc.stdlib_abi);
    add("crt", tc.crt);
    if (tc.family != Family::Msvc && tc.family != Family::ClangCl) add("eh", tc.eh);
    if (tc.is_mingw()) add("threads", tc.threads);
    add("std", std);
    add("runtime", profile.runtime);
    if (tc.family == Family::Msvc || tc.family == Family::ClangCl) add("debug-runtime", profile.debug_runtime ? "true" : "false");
    add("pic", tc.os == Os::Windows ? "false" : "true");
    add("sanitizers", str::join(sanitizers, ","));
    add("defines.abi", str::join(abi_defines, ","));
    add("exceptions", has_flag("-fno-exceptions") ? "off" : "on");
    add("rtti", has_flag("-fno-rtti") ? "off" : "on");

    std::sort(fp.inputs.begin(), fp.inputs.end());
    fp.hash = Sha256::hex(fp.canonical());
    return fp;
}

}  // namespace cpm::toolchain
