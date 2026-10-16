#include <algorithm>
#include <format>

#include "core/error.hpp"
#include "toolchain/driver.hpp"

namespace cpm::toolchain {

namespace {

// GCC and Clang (including MinGW and Apple Clang).
class GccDriver final : public Driver {
public:
    explicit GccDriver(const Toolchain& tc) : tc_(tc) {}

    std::vector<NinjaRule> rules() const override {
        // macOS's BSD ar has no @file response files; its command lines may be long enough (ARG_MAX ~1 MB).
        NinjaRule ar = tc_.os == Os::MacOS ? NinjaRule{"ar", "$ar rcs $out $in", "AR $out", "", "", "", ""}
                                           : NinjaRule{"ar", "$ar rcs $out @$out.rsp", "AR $out", "", "", "$out.rsp", "$in"};
        return {
            {"cxx", "$cxx -MD -MF $out.d $cxxflags $defines $incs -c $in -o $out", "CXX $in", "$out.d", "gcc", "", ""},
            ar,
            {"link", "$cxx @$out.rsp -o $out $ldflags", "LINK $out", "", "", "$out.rsp", "$in $libs"},
            {"link_shared", "$cxx -shared @$out.rsp -o $out $ldflags", "LINK $out", "", "", "$out.rsp", "$in $libs"},
        };
    }

    ProfileFlags profile_flags(const manifest::Profile& p, const std::string& std_flag, bool pic) const override {
        ProfileFlags f;
        f.compile.push_back(std_flag);
        f.compile.push_back(p.opt == "s" ? "-Os" : p.opt == "z" ? "-Oz" : "-O" + p.opt);
        if (p.debug) f.compile.push_back("-g");
        if (p.lto) {
            std::string lto = tc_.family == Family::Gcc ? "-flto=auto" : "-flto";
            f.compile.push_back(lto);
            f.link.push_back(lto);
        }

        std::vector<std::string> sanitizers = supported_sanitizers(p.sanitizers, f.notes);
        if (!sanitizers.empty()) {
            std::string joined;
            for (const auto& s : sanitizers) joined += (joined.empty() ? "" : ",") + s;
            f.compile.push_back("-fsanitize=" + joined);
            f.compile.push_back("-fno-omit-frame-pointer");
            f.link.push_back("-fsanitize=" + joined);
        }

        if (p.warnings == "none") {
            f.compile.push_back("-w");
        } else {
            f.compile.push_back("-Wall");
            f.compile.push_back("-Wextra");
            if (p.warnings == "strict") {
                for (const char* w : {"-Wpedantic", "-Wshadow", "-Wconversion", "-Wsign-conversion",
                                      "-Wnon-virtual-dtor", "-Woverloaded-virtual"})
                    f.compile.push_back(w);
            }
        }
        if (p.werror) f.compile.push_back("-Werror");
        if (pic && tc_.os != Os::Windows) f.compile.push_back("-fPIC");
        // Always on: Ninja strips colour codes itself when its output is not a terminal, and a stable
        // command line avoids rebuilding everything when the colour setting changes.
        f.compile.push_back("-fdiagnostics-color=always");

        if (p.runtime == "static") {
            if (tc_.os == Os::Windows) {
                f.link.push_back("-static");
            } else if (tc_.os == Os::MacOS) {
                f.notes.push_back("\"runtime\": \"static\" is not supported on macOS and was ignored");
            } else {
                f.link.push_back("-static-libstdc++");
                f.link.push_back("-static-libgcc");
            }
        }
        if (p.debug_runtime) f.notes.push_back("\"debugRuntime\" only applies to MSVC and was ignored");
        return f;
    }

    std::vector<std::string> sanitizers(const manifest::Profile& p) const override {
        std::vector<std::string> ignored;
        return supported_sanitizers(p.sanitizers, ignored);
    }

    std::string define_flag(std::string_view define) const override { return "-D" + std::string(define); }
    std::string include_flag(std::string_view dir) const override { return "-I" + std::string(dir); }
    std::vector<std::string> lib_flags(std::string_view dir, std::string_view name) const override {
        return {"-L" + std::string(dir), "-l" + std::string(name)};
    }
    std::string system_lib_flag(std::string_view name) const override { return "-l" + std::string(name); }
    std::vector<std::string> shared_link_flags(std::string_view file_name, std::string_view import_lib) const override {
        if (tc_.os == Os::Windows) return {"-Wl,--out-implib," + std::string(import_lib)};
        if (tc_.os == Os::MacOS) return {"-dynamiclib", "-Wl,-install_name,@rpath/" + std::string(file_name)};
        return {"-Wl,-soname," + std::string(file_name)};
    }

    std::string object_ext() const override { return ".o"; }
    std::string exe_file(std::string_view name) const override {
        return tc_.os == Os::Windows ? std::string(name) + ".exe" : std::string(name);
    }
    std::string static_lib_file(std::string_view name) const override { return std::format("lib{}.a", name); }
    std::string shared_lib_file(std::string_view name) const override {
        switch (tc_.os) {
            case Os::Windows: return std::format("lib{}.dll", name);
            case Os::MacOS: return std::format("lib{}.dylib", name);
            default: return std::format("lib{}.so", name);
        }
    }
    std::optional<std::string> import_lib_file(std::string_view name) const override {
        if (tc_.os == Os::Windows) return std::format("lib{}.dll.a", name);
        return std::nullopt;
    }

private:
    std::vector<std::string> supported_sanitizers(const std::vector<std::string>& wanted,
                                                  std::vector<std::string>& notes) const {
        std::vector<std::string> out;
        for (const auto& s : wanted) {
            std::string why;
            if (tc_.os == Os::Windows) why = "sanitizers are not available for MinGW toolchains";
            else if (s == "memory" && tc_.family != Family::Clang) why = "the memory sanitizer needs Clang on Linux";
            else if (s == "leak" && tc_.os == Os::MacOS) why = "the leak sanitizer is not available on macOS";
            else if (s == "thread" && std::find(wanted.begin(), wanted.end(), "address") != wanted.end())
                why = "the thread sanitizer cannot be combined with the address sanitizer";
            if (why.empty()) out.push_back(s);
            else notes.push_back(std::format("sanitizer \"{}\" dropped: {}", s, why));
        }
        return out;
    }

    Toolchain tc_;
};

}  // namespace

std::unique_ptr<Driver> make_driver(const Toolchain& tc) {
    switch (tc.family) {
        case Family::Gcc:
        case Family::Clang:
        case Family::AppleClang: return std::make_unique<GccDriver>(tc);
        case Family::Msvc:
        case Family::ClangCl: break;
    }
    throw Error(std::format("no driver for {} yet", to_string(tc.family)), "use g++ or clang++");
}

}  // namespace cpm::toolchain
