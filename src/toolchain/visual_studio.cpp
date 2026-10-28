#include "toolchain/visual_studio.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "json/parse.hpp"
#include "toolchain/probe_cache.hpp"

namespace cpm::toolchain {

namespace vs {

namespace {

// Paths compare without case on Windows, and without a trailing separator.
std::string path_key(std::string_view path) {
    std::string key = str::to_lower(str::trim(path));
    std::replace(key.begin(), key.end(), '\\', '/');
    while (key.size() > 1 && key.back() == '/') key.pop_back();
    return key;
}

std::vector<std::string> path_list(std::string_view value) {
    std::vector<std::string> out;
    for (const auto& part : str::split(value, ';')) {
        if (!str::trim(part).empty()) out.emplace_back(str::trim(part));
    }
    return out;
}

// "14.51.36231" before "14.44.35207": toolsets newest first.
bool newer_toolset(const std::string& a, const std::string& b) {
    auto numbers = [](const std::string& s) {
        std::vector<int> out;
        for (const auto& part : str::split(s, '.')) out.push_back(std::atoi(part.c_str()));
        return out;
    };
    return numbers(a) > numbers(b);
}

// The entry of `dir` named `name`, compared without case, as it is spelled on disk.
std::optional<fs::path> child(const fs::path& dir, std::string_view name) {
    for (const auto& e : fsx::list_dir(dir)) {
        if (str::to_lower(e.name) == str::to_lower(name)) return dir / paths::from_utf8(e.name);
    }
    return std::nullopt;
}

#ifdef _WIN32
// The installations as cached in toolchains.json, with the instance directory's stamp they were found under.
json::Value to_json(const std::vector<VisualStudio>& list, const std::string& stamp) {
    json::Value items = json::Value::array();
    for (const auto& vs : list) {
        json::Value item = json::Value::object();
        item.set("path", paths::to_utf8(vs.path));
        item.set("name", vs.name);
        item.set("version", vs.version);
        items.push_back(std::move(item));
    }
    json::Value section = json::Value::object();
    section.set("stamp", stamp);
    section.set("installations", std::move(items));
    return section;
}

std::optional<std::vector<VisualStudio>> from_json(const json::Value& section, const std::string& stamp) {
    try {
        if (section.at("stamp").as_string() != stamp) return std::nullopt;
        std::vector<VisualStudio> list;
        for (const auto& item : section.at("installations").as_array()) {
            list.push_back({paths::from_utf8(item.at("path").as_string()), item.at("name").as_string(),
                            item.at("version").as_string(), ""});
        }
        return list;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// What changes when the Visual Studio Installer adds, changes or removes an installation: the instance directories
// and their state.json files. Empty when there is no installer.
std::string instances_stamp() {
    fs::path dir = paths::from_utf8(env::get("ProgramData").value_or("C:/ProgramData")) / "Microsoft" / "VisualStudio" /
                   "Packages" / "_Instances";
    std::string stamp;
    for (const auto& e : fsx::list_dir(dir)) {
        if (!e.is_dir) continue;
        std::error_code ec;
        fs::path state = dir / paths::from_utf8(e.name) / "state.json";
        auto size = fs::file_size(state, ec);
        auto time = fs::last_write_time(state, ec).time_since_epoch().count();
        stamp += std::format("{}:{}:{};", e.name, ec ? 0 : size, ec ? 0 : time);
    }
    return stamp;
}

std::vector<VisualStudio> run_vswhere() {
    fs::path vswhere = paths::from_utf8(env::get("ProgramFiles(x86)").value_or("C:/Program Files (x86)")) /
                       "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) return {};
    std::string component = host_arch() == "arm64" ? "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
                                                    : "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
    proc::Captured r = proc::capture(
        {.argv = {paths::to_utf8(vswhere), "-products", "*", "-requires", component, "-format", "json", "-utf8"}});
    if (r.exit_code != 0) {
        log::verbose(std::format("vswhere failed (exit code {}): {}", r.exit_code, r.err));
        return {};
    }
    try {
        return parse_vswhere(r.out);
    } catch (const Error& e) {
        log::verbose(std::format("could not read vswhere's answer: {}", e.what()));
        return {};
    }
}
#endif

}  // namespace

std::vector<VisualStudio> parse_vswhere(std::string_view json_text) {
    json::Value doc = json::parse(json_text, json::kStrict);
    if (!doc.is_array()) throw Error("vswhere's answer is not a JSON array");
    std::vector<VisualStudio> out;
    for (const auto& item : doc.as_array()) {
        const json::Value* path = item.find("installationPath");
        if (path == nullptr || !path->is_string()) continue;
        VisualStudio vs;
        vs.path = paths::from_utf8(path->as_string());
        if (const json::Value* name = item.find("displayName"); name && name->is_string()) vs.name = name->as_string();
        if (vs.name.empty()) vs.name = "Visual Studio";
        const json::Value* catalog = item.find("catalog");
        const json::Value* shown = catalog ? catalog->find("productDisplayVersion") : nullptr;
        if (shown && shown->is_string()) {
            vs.version = shown->as_string();
        } else if (const json::Value* v = item.find("installationVersion"); v && v->is_string()) {
            vs.version = v->as_string();
        }
        out.push_back(std::move(vs));
    }
    return out;
}

std::vector<VisualStudio> installations([[maybe_unused]] const fs::path& home) {
#ifdef _WIN32
    std::string stamp = instances_stamp();
    if (stamp.empty()) return {};  // no Visual Studio Installer on this machine
    if (auto section = cached_section("visualStudio", home)) {
        if (auto cached = from_json(*section, stamp)) return *cached;
    }
    std::vector<VisualStudio> found = run_vswhere();
    cache_section("visualStudio", to_json(found, stamp), home);
    return found;
#else
    return {};
#endif
}

std::string host_arch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

std::vector<fs::path> compiler_dirs(const VisualStudio& vs) {
    std::vector<std::pair<std::string, fs::path>> toolsets;
    fs::path msvc = vs.path / "VC" / "Tools" / "MSVC";
    const std::string arch = host_arch();
    for (const auto& e : fsx::list_dir(msvc)) {
        if (!e.is_dir) continue;
        auto bin = child(msvc / paths::from_utf8(e.name), "bin");
        auto host = bin ? child(*bin, "Host" + arch) : std::nullopt;
        auto target = host ? child(*host, arch) : std::nullopt;
        std::error_code ec;
        if (target && fs::is_regular_file(*target / "cl.exe", ec)) toolsets.emplace_back(e.name, *target);
    }
    std::sort(toolsets.begin(), toolsets.end(), [](const auto& a, const auto& b) { return newer_toolset(a.first, b.first); });
    std::vector<fs::path> out;
    for (const auto& [name, dir] : toolsets) out.push_back(dir);
    return out;
}

std::optional<VisualStudio> locate(const fs::path& cl, const std::vector<VisualStudio>& known) {
    // …/<installation>/VC/Tools/MSVC/<toolset>/bin/Host<arch>/<arch>/cl.exe
    fs::path bin = cl.parent_path().parent_path().parent_path();
    fs::path toolset = bin.parent_path();
    fs::path msvc = toolset.parent_path();
    fs::path tools = msvc.parent_path();
    fs::path vc = tools.parent_path();
    auto named = [](const fs::path& p, std::string_view name) { return str::to_lower(paths::to_utf8(p.filename())) == name; };
    if (!named(bin, "bin") || !named(msvc, "msvc") || !named(tools, "tools") || !named(vc, "vc")) return std::nullopt;
    VisualStudio vs;
    vs.path = vc.parent_path();
    vs.toolset = paths::to_utf8(toolset.filename());
    vs.name = "Visual Studio";
    for (const auto& k : known) {
        if (path_key(paths::to_utf8(k.path)) == path_key(paths::to_utf8(vs.path))) {
            vs.name = k.name;
            vs.version = k.version;
        }
    }
    return vs;
}

std::map<std::string, std::string> parse_environment(std::string_view set_output, std::string_view inherited_path) {
    std::map<std::string, std::string> env;
    std::string path;
    for (const auto& line : str::split_lines(set_output)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string name = str::to_lower(std::string_view(line).substr(0, eq));
        std::string value = line.substr(eq + 1);
        if (name == "include") env["INCLUDE"] = value;
        else if (name == "lib") env["LIB"] = value;
        else if (name == "libpath") env["LIBPATH"] = value;
        else if (name == "external_include") env["EXTERNAL_INCLUDE"] = value;
        else if (name == "path") path = value;
    }
    std::vector<std::string> inherited;
    for (const auto& dir : path_list(inherited_path)) inherited.push_back(path_key(dir));
    std::vector<std::string> added;
    for (const auto& dir : path_list(path)) {
        std::string key = path_key(dir);
        bool known = std::find(inherited.begin(), inherited.end(), key) != inherited.end();
        bool repeated = std::any_of(added.begin(), added.end(), [&](const std::string& a) { return path_key(a) == key; });
        if (!known && !repeated) added.push_back(dir);
    }
    env["PATH"] = str::join(added, ";");
    return env;
}

std::map<std::string, std::string> capture_environment(const VisualStudio& vs) {
    fs::path bat = vs.path / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    std::error_code ec;
    if (!fs::is_regular_file(bat, ec)) {
        throw Error(std::format("{} has no vcvarsall.bat ({})", vs.name, paths::to_utf8(bat)),
                    "repair the installation in the Visual Studio Installer");
    }
    log::verbose(std::format("capturing the environment of MSVC toolset {} ({})", vs.toolset, vs.name));
    // /s: cmd removes the outer quotes and runs the rest as written (build.md §1.5).
    std::string line = std::format("call \"{}\" {} -vcvars_ver={} >nul 2>nul && set", paths::to_native_utf8(bat),
                                   host_arch(), vs.toolset);
    proc::Command cmd;
    cmd.argv = {env::get("COMSPEC").value_or("cmd.exe"), "/d", "/s", "/c", '"' + line + '"'};
    cmd.verbatim_args = true;
    proc::Captured r = proc::capture(cmd);
    std::map<std::string, std::string> env = parse_environment(r.out, env::get("PATH").value_or(""));
    if (r.exit_code != 0 || env["INCLUDE"].empty() || env["LIB"].empty()) {
        throw Error(std::format("setting up MSVC toolset {} of {} failed (vcvarsall.bat exit code {})", vs.toolset, vs.name,
                                r.exit_code),
                    "check in the Visual Studio Installer that the C++ tools and a Windows SDK are installed");
    }
    return env;
}

bool environment_intact(const std::map<std::string, std::string>& env) {
    // Captured before CPM 0.4.9, without EXTERNAL_INCLUDE: capture it again (build.md §1.5).
    if (!env.contains("EXTERNAL_INCLUDE")) return false;
    for (const char* name : {"INCLUDE", "LIB"}) {
        auto it = env.find(name);
        if (it == env.end()) return false;
        for (const auto& dir : path_list(it->second)) {
            std::error_code ec;
            if (!fs::is_directory(paths::from_utf8(dir), ec)) return false;
        }
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> process_variables(const std::map<std::string, std::string>& env,
                                                                   std::string_view inherited_path) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* name : {"INCLUDE", "LIB", "LIBPATH", "EXTERNAL_INCLUDE"}) {
        auto it = env.find(name);
        out.emplace_back(name, it == env.end() ? std::string() : it->second);
    }
    auto path = env.find("PATH");
    std::string dirs = path == env.end() ? std::string() : path->second;
    std::string joined = dirs;
    if (!inherited_path.empty()) joined += (joined.empty() ? "" : ";") + std::string(inherited_path);
    out.emplace_back("PATH", joined);
    return out;
}

std::optional<fs::path> bundled_ninja(const fs::path& home) {
    for (const auto& vs : installations(home)) {
        fs::path ninja = vs.path / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe";
        std::error_code ec;
        if (fs::is_regular_file(ninja, ec)) return ninja;
    }
    return std::nullopt;
}

}  // namespace vs

std::vector<std::pair<std::string, std::string>> process_environment(const Toolchain& tc) {
    if (tc.family != Family::Msvc || tc.env.empty()) return {};
    return vs::process_variables(tc.env, env::get("PATH").value_or(""));
}

std::vector<fs::path> runtime_dirs(const Toolchain& tc) {
    if (tc.os != Os::Windows) return {};
    if (tc.family != Family::Msvc) return {tc.cxx.parent_path()};
    std::vector<fs::path> dirs;
    auto it = tc.env.find("PATH");
    if (it != tc.env.end()) {
        for (const auto& dir : vs::path_list(it->second)) dirs.push_back(paths::from_utf8(dir));
    }
    return dirs;
}

}  // namespace cpm::toolchain
