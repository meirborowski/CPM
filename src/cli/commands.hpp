#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cli/args.hpp"

namespace cpm::cli {

enum class ColorMode { Auto, Always, Never };

// State shared by all commands, prepared by run() from global options and the environment.
struct Context {
    std::filesystem::path cwd;   // after -C
    ColorMode color_mode = ColorMode::Auto;
};

// Entry point: parses argv (without the program name), dispatches, and returns the process exit code.
int run(const std::vector<std::string>& argv);

int cmd_init(const ParsedArgs& args, Context& ctx);
int cmd_install(const ParsedArgs& args, Context& ctx);
int cmd_remove(const ParsedArgs& args, Context& ctx);
int cmd_update(const ParsedArgs& args, Context& ctx);
int cmd_build(const ParsedArgs& args, Context& ctx);
int cmd_run(const ParsedArgs& args, Context& ctx);
int cmd_test(const ParsedArgs& args, Context& ctx);
int cmd_publish(const ParsedArgs& args, Context& ctx);

}  // namespace cpm::cli
