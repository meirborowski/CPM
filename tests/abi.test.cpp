#include "manifest/profile.hpp"
#include "support/check.hpp"
#include "toolchain/abi.hpp"

using namespace cpm;

namespace {

toolchain::Toolchain mingw() {
    toolchain::Toolchain tc;
    tc.family = toolchain::Family::Gcc;
    tc.version = "13.2.0";
    tc.target = "x86_64-w64-mingw32";
    tc.os = Os::Windows;
    tc.arch = "x86_64";
    tc.stdlib = "libstdc++";
    tc.stdlib_abi = "cxx11";
    tc.crt = "ucrt";
    tc.eh = "seh";
    tc.threads = "posix";
    return tc;
}

std::string hash_of(const toolchain::Toolchain& tc, const manifest::Profile& p, std::vector<std::string> defines = {},
                    std::vector<std::string> flags = {}) {
    auto driver = toolchain::make_driver(tc);
    return toolchain::fingerprint(tc, *driver, p, "c++23", defines, flags).hash;
}

}  // namespace

TEST(matches_the_spec_example) {
    // docs/spec/abi.md §3: the canonical input text for this host's toolchain and the debug profile.
    auto tc = mingw();
    auto driver = toolchain::make_driver(tc);
    auto fp = toolchain::fingerprint(tc, *driver, manifest::builtin_profiles().at("debug"), "c++23", {}, {});
    CHECK_EQ(fp.canonical(),
             "abi.scheme=1\narch=x86_64\ncompiler=gcc\ncompiler.version=13.2.0\ncrt=ucrt\ndefines.abi=\neh=seh\n"
             "exceptions=on\nos=windows\npic=false\nrtti=on\nruntime=dynamic\nsanitizers=\nstd=c++23\n"
             "stdlib=libstdc++\nstdlib.abi=cxx11\ntarget=x86_64-w64-mingw32\nthreads=posix\n");
    CHECK_EQ(fp.hash.size(), 64u);
    CHECK_EQ(fp.dir(), fp.hash.substr(0, 16));
}

TEST(debug_and_release_share_an_abi) {
    auto profiles = manifest::builtin_profiles();
    CHECK_EQ(hash_of(mingw(), profiles.at("debug")), hash_of(mingw(), profiles.at("release")));
}

TEST(abi_relevant_settings_change_the_hash) {
    auto tc = mingw();
    manifest::Profile base = manifest::builtin_profiles().at("debug");
    std::string h = hash_of(tc, base);

    manifest::Profile static_rt = base;
    static_rt.runtime = "static";
    CHECK(hash_of(tc, static_rt) != h);

    CHECK(hash_of(tc, base, {"_GLIBCXX_DEBUG"}) != h);
    CHECK(hash_of(tc, base, {"MY_FEATURE=1"}) == h);  // not ABI-relevant
    CHECK(hash_of(tc, base, {}, {"-fno-exceptions"}) != h);

    auto other = tc;
    other.version = "13.3.0";
    CHECK(hash_of(other, base) != h);

    // MinGW drops sanitizers, so asking for one does not change the binary interface there.
    manifest::Profile asan = base;
    asan.sanitizers = {"address"};
    CHECK(hash_of(tc, asan) == h);
    auto linux_tc = tc;
    linux_tc.os = Os::Linux;
    CHECK(hash_of(linux_tc, asan) != hash_of(linux_tc, base));
}

TEST(abi_define_list) {
    CHECK(toolchain::is_abi_define("_GLIBCXX_USE_CXX11_ABI=0"));
    CHECK(toolchain::is_abi_define("_LIBCPP_ABI_FOO"));
    CHECK(!toolchain::is_abi_define("NDEBUG"));
}
