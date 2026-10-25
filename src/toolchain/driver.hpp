#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "manifest/profile.hpp"
#include "toolchain/language.hpp"
#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

// A Ninja rule. Commands may use $cxx, $cc, $ar, $cxxflags, $cflags, $defines, $incs, $ldflags and $libs.
struct NinjaRule {
    std::string name;
    std::string command;
    std::string description;
    std::string depfile;
    std::string deps;
    std::string rspfile;
    std::string rspfile_content;
};

struct ProfileFlags {
    std::vector<std::string> compile;
    std::vector<std::string> link;
    std::vector<std::string> notes;  // settings that were dropped, for warnings
};

// Translates the abstract build model into one compiler family's flags and file names
// (docs/spec/build.md §2-3).
class Driver {
public:
    virtual ~Driver() = default;

    virtual std::vector<NinjaRule> rules() const = 0;  // cxx, cc, ar, link, link_shared
    // The profile's flags for compiling `language` with the standard `std_flag` (build.md §3). The link flags don't
    // depend on the language: every link uses the C++ driver (build.md §1.2).
    virtual ProfileFlags profile_flags(const manifest::Profile& profile, Language language, const std::string& std_flag,
                                       bool pic) const = 0;
    // The profile's sanitizers that this toolchain supports (the others are dropped with a warning).
    virtual std::vector<std::string> sanitizers(const manifest::Profile& profile) const = 0;

    virtual std::string define_flag(std::string_view define) const = 0;
    virtual std::string include_flag(std::string_view dir) const = 0;
    virtual std::vector<std::string> lib_flags(std::string_view dir, std::string_view link_name) const = 0;
    virtual std::string system_lib_flag(std::string_view name) const = 0;
    // Extra link flags when producing a shared library. `file_name`: the library's file name (shared_lib_file),
    // recorded as its soname / install name so that dependents find it through their rpath. `import_lib`: where
    // to put the import library.
    virtual std::vector<std::string> shared_link_flags(std::string_view file_name, std::string_view import_lib) const = 0;

    virtual std::string object_ext() const = 0;
    virtual std::string exe_file(std::string_view name) const = 0;
    virtual std::string static_lib_file(std::string_view name) const = 0;
    virtual std::string shared_lib_file(std::string_view name) const = 0;
    virtual std::optional<std::string> import_lib_file(std::string_view name) const = 0;
};

std::unique_ptr<Driver> make_driver(const Toolchain& tc);

}  // namespace cpm::toolchain
