#include <algorithm>
#include <format>

#include "core/error.hpp"
#include "toolchain/driver.hpp"

namespace cpm::toolchain {

namespace {

// The warnings /W4 leaves off that match GCC's strict set (build.md §3): conversions that lose data or change sign,
// a class with virtual functions and a non-virtual destructor, a virtual function hidden by an overload, and
// suspicious comparisons and casts.
constexpr const char* kStrictWarnings[] = {"/w14242", "/w14254", "/w14365", "/w14826", "/w14265", "/w14263",
                                           "/w14287", "/w14296", "/w14905", "/w14906", "/w14928"};

// Visual Studio's cl.exe, with lib.exe and link.exe from its directory.
class MsvcDriver final : public Driver {
public:
    explicit MsvcDriver(const Toolchain& tc) : tc_(tc) {}

    std::vector<NinjaRule> rules() const override {
        // /showIncludes lists the headers on stdout; Ninja keeps them as dependencies and drops those lines, as well
        // as the source's name, which cl.exe prints for every file.
        return {
            {"cxx", "$cxx /nologo /showIncludes $cxxflags $defines $incs /c $in /Fo$out", "CXX $in", "", "msvc", "", "",
             tc_.deps_prefix},
            {"cc", "$cc /nologo /showIncludes $cflags $defines $incs /c $in /Fo$out", "CC $in", "", "msvc", "", "",
             tc_.deps_prefix},
            {"ar", "$ar /nologo /OUT:$out @$out.rsp", "AR $out", "", "", "$out.rsp", "$in", ""},
            // /SUBSYSTEM:CONSOLE lets main() come from a library, as cpm-test's does: without it link.exe looks for
            // main() only in the objects it is given.
            {"link", "$linker /nologo /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /OUT:$out @$out.rsp $ldflags", "LINK $out", "", "",
             "$out.rsp", "$in $libs", ""},
            // A DLL exports what its code marks with its <NAME>_API (manifest.md §4.1); /IMPLIB comes from
            // shared_link_flags.
            {"link_shared", "$linker /nologo /INCREMENTAL:NO /DLL /OUT:$out @$out.rsp $ldflags", "LINK $out", "", "",
             "$out.rsp", "$in $libs", ""},
        };
    }

    ProfileFlags profile_flags(const manifest::Profile& p, Language language, const std::string& std_flag,
                               bool) const override {
        ProfileFlags f;
        if (!std_flag.empty()) f.compile.push_back(std_flag);  // C89 and C99 are MSVC's default C
        f.compile.push_back(p.opt == "0" ? "/Od" : (p.opt == "s" || p.opt == "z") ? "/O1" : "/O2");
        if (p.debug) {
            // In the object files rather than a shared .pdb, so parallel compiles never compete for one file.
            f.compile.push_back("/Z7");
            f.link.push_back("/DEBUG");
        }
        if (p.lto) {
            f.compile.push_back("/GL");
            f.link.push_back("/LTCG");
        }
        for (const auto& s : sanitizers(p)) f.compile.push_back("/fsanitize=" + s);
        for (const auto& s : p.sanitizers) {
            if (s != "address") f.notes.push_back(std::format("sanitizer \"{}\" dropped: MSVC only has the address sanitizer", s));
        }

        if (p.warnings == "none") {
            f.compile.push_back("/W0");
        } else {
            f.compile.push_back("/W4");
            if (p.warnings == "strict") f.compile.insert(f.compile.end(), std::begin(kStrictWarnings), std::end(kStrictWarnings));
        }
        if (p.werror) f.compile.push_back("/WX");

        std::string runtime = p.runtime == "static" ? "/MT" : "/MD";
        if (p.debug_runtime) runtime += 'd';
        f.compile.push_back(runtime);

        // /external:W0: nothing from the toolchain's own headers (EXTERNAL_INCLUDE), as GCC and Clang report nothing
        // from system headers (build.md §1.5).
        for (const char* flag : {"/utf-8", "/Zc:preprocessor", "/bigobj", "/external:W0"}) f.compile.push_back(flag);
        if (language == Language::Cxx) {
            for (const char* flag : {"/EHsc", "/Zc:__cplusplus", "/permissive-"}) f.compile.push_back(flag);
        }
        return f;
    }

    std::vector<std::string> sanitizers(const manifest::Profile& p) const override {
        std::vector<std::string> out;
        if (std::find(p.sanitizers.begin(), p.sanitizers.end(), "address") != p.sanitizers.end()) out.push_back("address");
        return out;
    }

    std::string define_flag(std::string_view define) const override { return "/D" + std::string(define); }
    std::string include_flag(std::string_view dir) const override { return "/I" + std::string(dir); }
    std::vector<std::string> lib_flags(std::string_view dir, std::string_view name) const override {
        return {"/LIBPATH:" + std::string(dir), std::string(name) + ".lib"};
    }
    std::string system_lib_flag(std::string_view name) const override {
        return name.ends_with(".lib") ? std::string(name) : std::string(name) + ".lib";
    }
    std::vector<std::string> shared_link_flags(std::string_view, std::string_view import_lib) const override {
        return {"/IMPLIB:" + std::string(import_lib)};
    }

    std::string object_ext() const override { return ".obj"; }
    std::string exe_file(std::string_view name) const override { return std::string(name) + ".exe"; }
    std::string static_lib_file(std::string_view name) const override { return std::string(name) + ".lib"; }
    std::string shared_lib_file(std::string_view name) const override { return std::string(name) + ".dll"; }
    std::optional<std::string> import_lib_file(std::string_view name) const override { return std::string(name) + ".lib"; }

private:
    Toolchain tc_;
};

}  // namespace

std::unique_ptr<Driver> make_msvc_driver(const Toolchain& tc) { return std::make_unique<MsvcDriver>(tc); }

}  // namespace cpm::toolchain
