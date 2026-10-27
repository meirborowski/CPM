#include "run/run.hpp"

#include "core/env.hpp"
#include "core/paths.hpp"

namespace cpm::run {

std::vector<std::pair<std::string, std::string>> variables(const Environment& env) {
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::string path = paths::to_native_utf8(env.bin_dir) + sep + paths::to_native_utf8(env.cpm_exe.parent_path());
    for (const auto& dir : env.runtime_dirs) path += sep + paths::to_native_utf8(dir);
    if (auto existing = env::get("PATH"); existing && !existing->empty()) path += sep + *existing;
    return {
        {"PATH", path},
        {"CPM_PROJECT_ROOT", paths::to_native_utf8(env.root)},
        {"CPM_PROFILE", env.profile},
        {"CPM_PACKAGE_NAME", env.package_name},
        {"CPM_PACKAGE_VERSION", env.package_version},
        {"CPM_BIN_DIR", paths::to_native_utf8(env.bin_dir)},
        {"CPM_EXE", paths::to_native_utf8(env.cpm_exe)},
    };
}

proc::Command script_command(const std::string& script, const std::vector<std::string>& args, const Environment& env) {
    proc::Command cmd;
    cmd.cwd = env.root;
    cmd.env_set = variables(env);
#ifdef _WIN32
    // /s: cmd removes the outer quotes and runs the rest exactly as written. The appended arguments are
    // quoted for the program and escaped for cmd itself.
    std::string line = script;
    for (const auto& a : args) line += ' ' + proc::quote_cmd(a);
    std::string comspec = env::get("COMSPEC").value_or("cmd.exe");
    cmd.argv = {comspec, "/d", "/s", "/c", '"' + line + '"'};
    cmd.verbatim_args = true;
#else
    // The arguments become "$@" of the script, so the shell never re-parses them.
    cmd.argv = {"/bin/sh", "-c", script + " \"$@\"", "sh"};
    cmd.argv.insert(cmd.argv.end(), args.begin(), args.end());
#endif
    return cmd;
}

proc::Command program_command(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
                              const Environment& env) {
    proc::Command cmd;
    cmd.argv = {paths::to_utf8(exe)};
    cmd.argv.insert(cmd.argv.end(), args.begin(), args.end());
    cmd.cwd = cwd;
    cmd.env_set = variables(env);
    return cmd;
}

}  // namespace cpm::run
