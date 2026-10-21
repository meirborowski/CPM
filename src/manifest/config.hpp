#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace cpm::manifest {

struct RegistryConfig {
    std::string url;
    bool allow_http = false;
};

// $CPM_HOME/config.jsonc (docs/spec/cli.md §5). Every field is optional.
struct UserConfig {
    std::filesystem::path file;
    std::map<std::string, RegistryConfig> registries;
    std::optional<std::string> cxx;
    std::optional<std::string> toolchain;  // the machine's compiler request (build.md §1.1)
    std::optional<std::string> ninja;
    std::optional<int> jobs;
    std::optional<std::string> color;
    std::optional<std::string> link_mode;
    std::vector<Diagnostic> warnings;
};

// Loads the configuration from `home` (normally $CPM_HOME). A missing file yields defaults.
// Throws DiagnosticsError for invalid content.
UserConfig load_user_config(const std::filesystem::path& home);

// Sets (or, with nullopt, removes) a top-level string in $CPM_HOME/config.jsonc, creating the file if needed and
// editing it in place, so comments survive.
void set_user_config(const std::filesystem::path& home, const std::string& key, const std::optional<std::string>& value);

}  // namespace cpm::manifest
