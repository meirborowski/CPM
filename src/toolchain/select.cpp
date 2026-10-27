#include "toolchain/select.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "toolchain/probe.hpp"
#include "toolchain/visual_studio.hpp"

namespace cpm::toolchain {

namespace {

// "g++", "clang++", "g++-14", "clang++-18" (".exe" on Windows), and on Windows "cl": the family's program name and
// its version suffix.
std::optional<std::pair<std::string, int>> compiler_name(std::string name) {
#ifdef _WIN32
    name = str::to_lower(name);
    if (!name.ends_with(".exe")) return std::nullopt;
    name.resize(name.size() - 4);
    if (name == "cl") return std::pair{name, 0};
#endif
    for (const char* base : {"clang++", "g++"}) {
        std::string_view b = base;
        if (name == b) return std::pair{std::string(b), 0};
        if (name.size() > b.size() + 1 && name.starts_with(b) && name[b.size()] == '-') {
            std::string_view digits = std::string_view(name).substr(b.size() + 1);
            if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }) && digits.size() <= 3) {
                return std::pair{std::string(b), std::stoi(std::string(digits))};
            }
        }
    }
    return std::nullopt;
}

std::string found_list(const std::vector<Found>& found) {
    std::string out;
    for (const auto& f : found) {
        if (!f.toolchain) continue;
        out += std::format("\n  {} ({}){}", f.toolchain->description(), paths::to_utf8(f.cxx),
                           f.problem.empty() ? "" : ": " + f.problem);
    }
    return out.empty() ? "\n  none" : out;
}

std::string install_hint(Family family) {
    if (family == Family::AppleClang) return "install Apple's Command Line Tools: xcode-select --install";
    if (family == Family::Msvc) {
        return "install Visual Studio or its Build Tools with the workload \"Desktop development with C++\"";
    }
#ifdef _WIN32
    if (family == Family::Clang) return "install llvm-mingw or MSYS2's mingw-w64-ucrt-x86_64-clang";
#endif
    return std::format("install one with: cpm setup --compiler {}", family == Family::Gcc ? "gcc" : "clang");
}

constexpr const char* kMsvcAbiHint =
    "use --toolchain msvc for Visual Studio's compiler, or a MinGW toolchain (g++ or clang++ from MSYS2 or llvm-mingw)";

// A compiler named by a program name or path: the rules of build.md §1.1 for --cxx, CXX and "cxx".
Toolchain named_compiler(const std::string& text, const std::string& source, const fs::path& home) {
    auto p = proc::find_program(text);
    if (!p) throw Error(std::format("compiler '{}' (from {}) not found", text, source), "check the name, or give a full path");
    if (str::to_lower(paths::to_utf8(p->stem())) == "clang-cl") {
        throw Error(std::format("'{}' is clang-cl, which CPM does not support", paths::to_utf8(*p)), kMsvcAbiHint);
    }
    Toolchain tc = probe_compiler(*p, home);
    if (targets_msvc(tc)) {
        throw Error(std::format("{} is a Clang that targets the MSVC ABI ({}); CPM uses Visual Studio's cl.exe for that",
                                paths::to_utf8(tc.cxx), tc.target),
                    kMsvcAbiHint);
    }
    return tc;
}

// The first of `requests` that a compiler on this machine meets.
Toolchain requested_compiler(const std::vector<std::string>& requests, const std::string& source, const fs::path& home) {
    // --toolchain and config.jsonc's "toolchain" may also name a compiler.
    if (requests.size() == 1 && !parse_request(requests[0])) return named_compiler(requests[0], source, home);
    std::vector<Found> found = list_compilers(home);
    std::optional<Family> first_family;
    for (const auto& text : requests) {
        auto request = parse_request(text);
        if (!request) continue;  // the manifest checks that the project lists only requests
        if (!first_family) first_family = request->family;
        if (const Found* f = best_match(found, *request)) return *f->toolchain;
    }
    std::string asked;
    for (const auto& r : requests) asked += std::format("{}\"{}\"", asked.empty() ? "" : " or ", r);
    throw Error(std::format("no compiler on this machine matches {} (from {}); CPM found:{}", asked, source, found_list(found)),
                first_family ? install_hint(*first_family) + ", or choose another with --toolchain" : std::string());
}

// The MSVC compilers of the Visual Studio installations on this machine (build.md §1.4).
std::vector<Found> visual_studio_compilers(const fs::path& home) {
    std::vector<Found> out;
    for (const auto& vs : vs::installations(home)) {
        for (const auto& dir : vs::compiler_dirs(vs)) {
            Found f;
            f.cxx = dir / "cl.exe";
            try {
                f.toolchain = probe_compiler(f.cxx, home);
            } catch (const Error& e) {
                log::verbose(std::format("skipping {}: {}", paths::to_utf8(f.cxx), e.what()));
                continue;
            }
            out.push_back(std::move(f));
        }
    }
    return out;
}

// clang++, then g++, on PATH; then on Windows the newest MSVC that Visual Studio has (build.md §1.1).
Toolchain automatic(const fs::path& home) {
    std::optional<Toolchain> msvc_abi_clang;
    for (const char* name : {"clang++", "g++"}) {
        auto p = proc::find_program(name);
        if (!p) continue;
        Toolchain tc = probe_compiler(*p, home);
        if (!targets_msvc(tc)) {
            tc.chosen_by = "found on PATH";
            return tc;
        }
        // Windows machines often have a clang++ that targets MSVC: move on.
        log::verbose(std::format("skipping {}: it targets the MSVC ABI ({})", paths::to_utf8(tc.cxx), tc.target));
        msvc_abi_clang = tc;
    }
    std::vector<Found> msvc = visual_studio_compilers(home);
    if (const Found* f = best_match(msvc, Request{Family::Msvc, std::nullopt, "msvc"})) {
        Toolchain tc = *f->toolchain;
        tc.chosen_by = "found in Visual Studio";
        return tc;
    }
    if (msvc_abi_clang) {
        throw Error(std::format("{} is a Clang that targets the MSVC ABI ({}); CPM uses Visual Studio's cl.exe for that",
                                paths::to_utf8(msvc_abi_clang->cxx), msvc_abi_clang->target),
                    "install Visual Studio's C++ tools or a MinGW toolchain; cpm setup installs one");
    }
#ifdef _WIN32
    const char* looked = "no C++ compiler found (looked for clang++ and g++ on PATH, and for Visual Studio's C++ tools)";
#else
    const char* looked = "no C++ compiler found (looked for clang++ and g++ on PATH)";
#endif
    throw Error(looked, "run cpm setup to install one, or choose one with --toolchain or CXX");
}

}  // namespace

std::optional<Request> parse_request(std::string_view text) {
    std::string_view t = str::trim(text);
    std::size_t space = t.find_first_of(" \t");
    std::string_view word = t.substr(0, space);
    Request r;
    if (word == "gcc") r.family = Family::Gcc;
    else if (word == "clang") r.family = Family::Clang;
    else if (word == "apple-clang") r.family = Family::AppleClang;
    else if (word == "msvc") r.family = Family::Msvc;
    else return std::nullopt;
    r.text = std::string(t);
    std::string_view rest = space == std::string_view::npos ? std::string_view() : str::trim(t.substr(space));
    if (!rest.empty()) {
        r.range = semver::Range::parse(rest);
        if (!r.range) {
            throw Error(std::format("invalid compiler version range \"{}\" in \"{}\"", rest, t),
                        "write a family and a range, such as \"gcc >=14\", \"clang 18\", \"gcc ^13.2\" or \"msvc >=19.44\"");
        }
    }
    return r;
}

bool matches(const Toolchain& tc, const Request& request) {
    if (tc.family != request.family) return false;
    if (!request.range) return true;
    auto v = semver::Version::parse(tc.version);
    return v && request.range->contains(*v);
}

std::vector<fs::path> compiler_dirs(const fs::path& home) {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    for (const auto& d : str::split(env::get("PATH").value_or(""), sep)) {
        if (!str::trim(d).empty()) dirs.push_back(paths::from_utf8(std::string(str::trim(d))));
    }
    for (const auto& tool : fsx::list_dir(home / "tools")) {
        if (!tool.is_dir) continue;
        dirs.push_back(home / "tools" / paths::from_utf8(tool.name) / "bin");
        dirs.push_back(home / "tools" / paths::from_utf8(tool.name) / "mingw64" / "bin");
    }
#ifdef _WIN32
    fs::path msys = paths::from_utf8(env::get("MSYS2_ROOT").value_or("C:/msys64"));
    for (const char* env_dir : {"ucrt64", "clang64", "mingw64"}) dirs.push_back(msys / env_dir / "bin");
    for (const auto& vs : vs::installations(home)) {
        for (const auto& dir : vs::compiler_dirs(vs)) dirs.push_back(dir);
    }
#elif defined(__APPLE__)
    dirs.push_back("/opt/homebrew/opt/llvm/bin");
    dirs.push_back("/usr/local/opt/llvm/bin");
#endif
    return dirs;
}

std::vector<fs::path> find_compilers(const std::vector<fs::path>& dirs) {
    std::vector<fs::path> out;
    std::set<std::string> seen;  // real files, so a program reached through two names counts once
    for (const auto& dir : dirs) {
        std::vector<std::pair<std::pair<std::string, int>, std::string>> here;
        for (const auto& e : fsx::list_dir(dir)) {
            if (e.is_dir) continue;
            if (auto n = compiler_name(e.name)) here.emplace_back(*n, e.name);
        }
        std::sort(here.begin(), here.end());  // clang++ before g++, unversioned before versioned, then by version
        for (const auto& [key, name] : here) {
            fs::path p = dir / paths::from_utf8(name);
            std::error_code ec;
            fs::path real = fs::weakly_canonical(p, ec);
            std::string id = paths::to_utf8(ec ? p : real);
#ifdef _WIN32
            id = str::to_lower(id);
#endif
            if (seen.insert(id).second) out.push_back(p);
        }
    }
    return out;
}

std::vector<Found> list_compilers(const fs::path& home) {
    std::vector<Found> out;
    for (const auto& cxx : find_compilers(compiler_dirs(home))) {
        Found f;
        f.cxx = cxx;
        try {
            f.toolchain = probe_compiler(cxx, home);
            if (targets_msvc(*f.toolchain)) {
                f.problem = std::format("targets the MSVC ABI ({}); CPM uses Visual Studio's cl.exe for that", f.toolchain->target);
            } else if (older_than_supported(*f.toolchain)) {
                f.note = std::format("older than CPM supports ({} {} or newer); fine for projects that need less than C++23",
                                     to_string(f.toolchain->family), minimum_version(f.toolchain->family));
            }
        } catch (const Error& e) {
            std::string what = e.what();
            f.problem = "can't be probed: " + what.substr(0, what.find('\n'));
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::string minimum_version(Family family) {
    switch (family) {
        case Family::Gcc: return "13";
        case Family::AppleClang: return "15";
        case Family::Msvc: return "19.40";
        default: return "17";
    }
}

bool older_than_supported(const Toolchain& tc) {
    auto version = semver::Version::parse(tc.version);
    std::string minimum = minimum_version(tc.family);
    while (std::count(minimum.begin(), minimum.end(), '.') < 2) minimum += ".0";
    auto oldest = semver::Version::parse(minimum);
    return version && oldest && *version < *oldest;
}

const Found* best_match(const std::vector<Found>& found, const Request& request) {
    const Found* best = nullptr;
    std::optional<semver::Version> best_version;
    for (const auto& f : found) {
        if (!f.toolchain || !f.problem.empty() || !matches(*f.toolchain, request)) continue;
        auto v = semver::Version::parse(f.toolchain->version);
        if (best == nullptr || (v && (!best_version || *v > *best_version))) {
            best = &f;
            best_version = v;
        }
    }
    return best;
}

Toolchain find_toolchain(const Selection& sel) {
    Toolchain tc;
    bool overrides_project = true;
    if (sel.command) {
        bool requests = sel.command->source != "--cxx";
        tc = requests ? requested_compiler({sel.command->text}, sel.command->source, sel.home)
                      : named_compiler(sel.command->text, sel.command->source, sel.home);
        tc.chosen_by = "from " + sel.command->source;
    } else if (auto cxx = env::get("CXX"); cxx && !cxx->empty()) {
        tc = named_compiler(*cxx, "CXX", sel.home);
        tc.chosen_by = "from CXX";
    } else if (!sel.project.empty()) {
        tc = requested_compiler(sel.project, "cpm.jsonc", sel.home);
        tc.chosen_by = "from cpm.jsonc";
        overrides_project = false;
    } else if (sel.config_cxx) {
        tc = named_compiler(*sel.config_cxx, "config.jsonc", sel.home);
        tc.chosen_by = "from config.jsonc";
    } else if (sel.config_toolchain) {
        tc = requested_compiler({*sel.config_toolchain}, "config.jsonc", sel.home);
        tc.chosen_by = "from config.jsonc";
    } else {
        tc = automatic(sel.home);  // says itself what chose it
    }
    // The command line and CXX win over the project, which may then get a compiler it didn't ask for (build.md §1.1).
    if (overrides_project && !sel.project.empty() && (sel.command || env::is_set("CXX"))) {
        bool accepted = std::any_of(sel.project.begin(), sel.project.end(), [&](const std::string& text) {
            auto r = parse_request(text);
            return r && matches(tc, *r);
        });
        if (!accepted) {
            std::string asked;
            for (const auto& r : sel.project) asked += std::format("{}\"{}\"", asked.empty() ? "" : " or ", r);
            log::warn(std::format("the project asks for {}, but {} chose {}", asked, sel.command ? sel.command->source : "CXX",
                                  tc.description()),
                      "using it anyway; the build may fail if the project needs what it asked for");
        }
    }
    return tc;
}

}  // namespace cpm::toolchain
