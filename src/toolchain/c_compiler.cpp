#include "toolchain/c_compiler.hpp"

#include <array>
#include <format>
#include <map>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "toolchain/probe.hpp"
#include "toolchain/probe_cache.hpp"

namespace cpm::toolchain {

namespace fs = std::filesystem;

std::optional<std::string> c_compiler_name(std::string_view cxx_file_name) {
    // clang++ comes first because it ends in g++.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> drivers = {
        {{"clang++", "clang"}, {"g++", "gcc"}, {"c++", "cc"}}};
    for (const auto& [cxx, c] : drivers) {
        auto pos = cxx_file_name.rfind(cxx);
        if (pos == std::string_view::npos) continue;
        return std::format("{}{}{}", cxx_file_name.substr(0, pos), c, cxx_file_name.substr(pos + cxx.size()));
    }
    return std::nullopt;
}

void find_c_compiler(Toolchain& tc, const fs::path& home) {
    if (tc.c) return;
    if (auto cached = cached_toolchain(tc.cxx, home); cached && cached->c) {
        tc.c = cached->c;
        return;
    }

    auto name = c_compiler_name(paths::to_utf8(tc.cxx.filename()));
    fs::path cc = name ? tc.cxx.parent_path() / paths::from_utf8(*name) : fs::path();
    std::error_code ec;
    if (!name || !fs::is_regular_file(cc, ec)) {
        std::string looked = name ? std::format(" (looked for {})", paths::to_utf8(cc)) : "";
        throw Error(std::format("this build compiles C, but there is no C compiler next to {}{}", paths::to_utf8(tc.cxx), looked),
                    "install the C compiler that comes with it, or choose a C++ compiler that has one ('cpm toolchain list')");
    }

    log::verbose(std::format("probing {}", paths::to_utf8(cc)));
    Toolchain probed = toolchain_from_probe(run_probe(cc, Language::C, home), cc);
    if (probed.family != tc.family || probed.version != tc.version || probed.target != tc.target) {
        throw Error(std::format("the C compiler {} is {} for {}, but the C++ compiler {} is {} for {}", paths::to_utf8(cc),
                                probed.description(), probed.target, paths::to_utf8(tc.cxx), tc.description(), tc.target),
                    "C and C++ objects must come from one toolchain: install the matching C compiler, or choose another "
                    "C++ compiler");
    }
    tc.c = CCompiler{.path = cc};
    cache_toolchain(tc, home);
}

std::string cstd_flag(Toolchain& tc, const std::string& cstd, const fs::path& home) {
    find_c_compiler(tc, home);
    std::map<std::string, std::string>& flags = tc.c->cstd_flags;
    if (auto it = flags.find(cstd); it != flags.end()) return it->second;

    static const std::map<std::string, std::string> provisional = {{"c23", "c2x"}, {"gnu23", "gnu2x"}};
    std::vector<std::string> spellings = {cstd};
    if (auto it = provisional.find(cstd); it != provisional.end()) spellings.push_back(it->second);
    for (const auto& spelling : spellings) {
        std::string flag = "-std=" + spelling;
        if (accepts_flag(tc.c->path, Language::C, flag, home)) {
            flags[cstd] = flag;
            cache_toolchain(tc, home);
            return flag;
        }
    }
    throw Error(std::format("the C compiler {} ({}) does not support {}", paths::to_utf8(tc.c->path), tc.description(), cstd),
                "use a newer compiler, or another \"cstd\" in the package's cpm.jsonc");
}

}  // namespace cpm::toolchain
