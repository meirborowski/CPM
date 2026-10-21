#include "cli/options.hpp"

#include <charconv>
#include <format>

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "install/installer.hpp"

namespace cpm::cli {

std::string profile_option(const ParsedArgs& args) {
    auto profile = args.value("profile");
    if (args.flag("release")) {
        if (profile && *profile != "release")
            throw UsageError(std::format("--release conflicts with --profile {}", *profile));
        return "release";
    }
    return profile.value_or("debug");
}

int jobs_option(const ParsedArgs& args, const manifest::UserConfig& config) {
    auto value = args.value("jobs");
    if (!value) return config.jobs.value_or(0);
    int jobs = 0;
    auto [p, ec] = std::from_chars(value->data(), value->data() + value->size(), jobs);
    if (ec != std::errc() || p != value->data() + value->size() || jobs < 1)
        throw UsageError(std::format("invalid --jobs '{}'", *value), "give a positive number, e.g. -j8");
    return jobs;
}

std::function<std::vector<build::Dependency>(const build::Project&)> dependency_provider(const manifest::UserConfig& config) {
    return [config](const build::Project& project) { return install::ensure(project, config); };
}

std::optional<toolchain::Choice> toolchain_option(const ParsedArgs& args) {
    auto toolchain = args.value("toolchain");
    auto cxx = args.value("cxx");
    if (toolchain && cxx) throw UsageError("use --toolchain or --cxx, not both", "--toolchain takes a compiler too: --toolchain g++-14");
    if (toolchain) return toolchain::Choice{*toolchain, "--toolchain"};
    if (cxx) return toolchain::Choice{*cxx, "--cxx"};
    return std::nullopt;
}

manifest::UserConfig load_config() {
    manifest::UserConfig config = manifest::load_user_config(paths::cpm_home());
    for (const auto& w : config.warnings) log::diagnostic(w);
    return config;
}

}  // namespace cpm::cli
