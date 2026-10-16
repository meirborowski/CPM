#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json/value.hpp"

namespace cpm::manifest {

class Checker;

// A fully resolved build profile (docs/spec/manifest.md §6).
struct Profile {
    std::string name;
    std::string opt = "0";            // "0" "1" "2" "3" "s" "z"
    bool debug = true;
    bool assertions = true;
    bool lto = false;
    bool werror = false;
    bool debug_runtime = false;
    std::vector<std::string> sanitizers;
    std::vector<std::string> defines;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
    std::string runtime = "dynamic";  // dynamic | static
    std::string warnings = "default"; // none | default | strict
};

// The built-in debug and release profiles.
std::map<std::string, Profile> builtin_profiles();

// Built-ins merged with the manifest's "profiles" object (may be null). Problems go to `checker`.
std::map<std::string, Profile> resolve_profiles(const json::Value* profiles, Checker& checker);

bool valid_define(std::string_view define);
bool valid_system_lib(std::string_view name);

}  // namespace cpm::manifest
