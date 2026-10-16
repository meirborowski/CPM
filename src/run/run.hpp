#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/proc.hpp"

namespace cpm::run {

namespace fs = std::filesystem;

// What scripts and programs started by `cpm run` learn about the package (docs/spec/cli.md, cpm run).
struct Environment {
    fs::path root;     // project root
    std::string profile;
    std::string package_name;
    std::string package_version;
    fs::path bin_dir;  // <root>/build/<profile>: the package's executable and libraries
    fs::path cpm_exe;  // the running cpm
};

// CPM_* variables plus PATH with the profile's bin directory and cpm's own directory in front.
std::vector<std::pair<std::string, std::string>> variables(const Environment& env);

// A manifest script with extra arguments appended, run by the system shell (cmd.exe /d /s /c on Windows,
// /bin/sh -c elsewhere) in the project root. Arguments reach the script exactly as given.
proc::Command script_command(const std::string& script, const std::vector<std::string>& args, const Environment& env);

// The built executable with its arguments, run in `cwd`.
proc::Command program_command(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
                              const Environment& env);

}  // namespace cpm::run
