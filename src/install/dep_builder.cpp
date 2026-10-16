#include "install/dep_builder.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "build/layout.hpp"
#include "build/ninja_writer.hpp"
#include "build/plan.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "install/fetch.hpp"
#include "install/store.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "toolchain/abi.hpp"
#include "version.hpp"

namespace cpm::install {

namespace {

// The settings dependencies are compiled with: optimised, quiet, and otherwise exactly what the ABI
// fingerprint encodes (build.md §5 step 4).
manifest::Profile dependency_profile(const manifest::Profile& root, const std::vector<std::string>& root_cxxflags) {
    manifest::Profile p = root;
    p.opt = "2";
    p.debug = false;
    p.assertions = false;
    p.lto = false;
    p.warnings = "none";
    p.werror = false;
    p.defines.clear();
    for (const auto& d : root.defines) {
        if (toolchain::is_abi_define(d)) p.defines.push_back(d);
    }
    p.cxxflags.clear();
    for (const auto& f : root_cxxflags) {
        if (f == "-fno-exceptions" || f == "-fno-rtti") p.cxxflags.push_back(f);
    }
    p.ldflags.clear();
    return p;
}

std::string tail(std::string_view text, std::size_t n) {
    auto lines = str::split_lines(text);
    std::size_t start = lines.size() > n ? lines.size() - n : 0;
    return str::join(std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(start), lines.end()), "\n");
}

std::string abs_utf8(const fs::path& p) { return paths::to_utf8(fs::absolute(p).lexically_normal()); }

// Under the library's lock: a complete ABI directory that failed library_intact lost files to something
// outside CPM, so it is removed and made again (store.md §2.3).
void discard_damaged(const fs::path& out_dir, std::string_view what) {
    std::error_code ec;
    if (!fs::exists(out_dir / ".complete", ec)) return;
    log::warn(std::format("the store library {} is missing files; making it again", what));
    fsx::remove_tree(out_dir);
}

}  // namespace

fs::path ensure_library(const build::Project& project, const lock::Package& package, const fs::path& entry,
                        const manifest::Manifest& m, const DependencyInputs& inputs, const fs::path& ninja) {
    const std::string abi = project.abi.dir();
    fs::path out_dir = entry / "lib" / abi;
    if (library_intact(out_dir)) return out_dir;

    std::string what = std::format("{} for abi {}", entry_name(package), abi);
    FileLock lock = FileLock::acquire(project.home / "locks" / (Sha256::hex(what).substr(0, 16) + ".lock"), what);
    if (library_intact(out_dir)) return out_dir;
    discard_damaged(out_dir, what);

    const toolchain::Toolchain& tc = project.toolchain;
    const toolchain::Driver& driver = *project.driver;
    manifest::PlatformOverrides platform = manifest::effective_platform(m, tc.os);
    build::Layout layout = build::infer_layout(m, platform);
    if (layout.kind != manifest::TargetKind::StaticLib) {
        throw Error(std::format("internal error: {} is not a static library", package.name));
    }

    log::status("Building", std::format("{} from source (abi {})", package.key(), abi));
    auto start = std::chrono::steady_clock::now();

    manifest::PlatformOverrides root_platform = manifest::effective_platform(project.manifest, tc.os);
    std::vector<std::string> root_cxxflags = project.profile.cxxflags;
    root_cxxflags.insert(root_cxxflags.end(), root_platform.cxxflags.begin(), root_platform.cxxflags.end());
    manifest::Profile profile = dependency_profile(project.profile, root_cxxflags);
    toolchain::ProfileFlags flags = driver.profile_flags(profile, project.std_flag, tc.os != Os::Windows);

    std::vector<std::string> incs = {driver.include_flag(abs_utf8(entry / "src"))};
    if (layout.include_dir_exists) incs.push_back(driver.include_flag(abs_utf8(entry / paths::from_utf8(m.include_dir))));
    for (const auto& dir : inputs.include_dirs) incs.push_back(driver.include_flag(abs_utf8(dir)));

    std::vector<std::string> define_list = {"NDEBUG"};
    for (const auto* list : std::initializer_list<const std::vector<std::string>*>{&profile.defines, &m.defines, &m.public_defines, &platform.defines,
                             &platform.public_defines, &inputs.public_defines}) {
        for (const auto& d : *list) {
            if (std::find(define_list.begin(), define_list.end(), d) == define_list.end()) define_list.push_back(d);
        }
    }
    std::vector<std::string> defines;
    for (const auto& d : define_list) defines.push_back(driver.define_flag(d));
    std::vector<std::string> cxxflags = flags.compile;
    cxxflags.insert(cxxflags.end(), profile.cxxflags.begin(), profile.cxxflags.end());

    fs::path sandbox = make_temp_dir(project.home, "build");
    std::string archive_name = driver.static_lib_file(package.name);
    build::ninja::Writer w;
    w.comment(std::format("Generated by cpm {} for {} (abi {}). Sandbox build of a store package.", version,
                          package.key(), abi));
    w.variable("ninja_required_version", "1.10");
    w.variable("cxx", build::ninja::args_value({paths::to_utf8(tc.cxx)}));
    w.variable("ar", build::ninja::args_value({paths::to_utf8(tc.ar)}));
    w.variable("cxxflags", build::ninja::args_value(cxxflags));
    w.variable("defines", build::ninja::args_value(defines));
    w.variable("incs", build::ninja::args_value(incs));
    w.newline();
    for (const auto& rule : driver.rules()) {
        if (rule.name == "cxx" || rule.name == "ar") w.rule(rule);
    }
    std::vector<std::string> objects;
    for (const auto& src : layout.sources) {
        std::string obj = build::object_path("obj", src, driver.object_ext());
        w.build({obj}, "cxx", {abs_utf8(entry / paths::from_utf8(src))});
        objects.push_back(obj);
    }
    w.build({archive_name}, "ar", objects);
    w.defaults({archive_name});
    fsx::write_file_atomic(sandbox / "build.ninja", w.str());

    proc::Captured r = proc::capture({.argv = {paths::to_utf8(ninja), "-f", "build.ninja"},
                                      .cwd = sandbox,
                                      .env_set = {{"NINJA_STATUS", "[%f/%t] "}, {"TERM", "dumb"}},
                                      .env_unset = {"CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"}},
                                     std::nullopt, true);
    fs::path log_file = project.home / "logs" / std::format("build-{}-{}.log", package.key(), abi);
    fsx::write_file_atomic(log_file, r.out + r.err);
    if (r.exit_code != 0) {
        if (!env::is_set("CPM_KEEP_SANDBOX")) fsx::remove_tree(sandbox);
        throw Error(std::format("building {} failed:\n{}", package.key(), tail(r.out + r.err, 40)),
                    std::format("full log: {}", paths::to_utf8(log_file)));
    }

    // Stage next to the destination, then rename into place.
    fs::path staging = entry / "lib" / (abi + ".tmp-" + sandbox.filename().string());
    fsx::create_dirs(staging);
    fs::copy_file(sandbox / archive_name, staging / archive_name, fs::copy_options::overwrite_existing);

    json::Value meta = json::Value::object();
    meta.set("metaVersion", 1);
    json::Value pkg = json::Value::object();
    pkg.set("name", package.name);
    pkg.set("version", package.version.to_string());
    if (const auto* git = std::get_if<lock::GitResolution>(&package.resolution)) pkg.set("integrity", "git:" + git->commit);
    meta.set("package", std::move(pkg));
    json::Value abi_json = json::Value::object();
    abi_json.set("hash", project.abi.hash);
    json::Value abi_inputs = json::Value::object();
    for (const auto& [k, v] : project.abi.inputs) abi_inputs.set(k, v);
    abi_json.set("inputs", std::move(abi_inputs));
    meta.set("abi", std::move(abi_json));
    json::Value artifact = json::Value::object();
    artifact.set("kind", "static-lib");
    artifact.set("file", archive_name);
    meta.set("artifacts", json::Value(json::Array{artifact}));
    meta.set("linkName", package.name);
    json::Value pub = json::Value::array();
    for (const auto& d : m.public_defines) pub.push_back(d);
    for (const auto& d : platform.public_defines) pub.push_back(d);
    meta.set("publicDefines", std::move(pub));
    json::Value sys = json::Value::array();
    for (const auto& l : m.system_libs) sys.push_back(l);
    for (const auto& l : platform.system_libs) sys.push_back(l);
    meta.set("systemLibs", std::move(sys));
    meta.set("builtBy", std::format("cpm {}", version));
    meta.set("origin", "source");
    fsx::write_file_atomic(staging / "meta.json", json::write(meta) + "\n");
    fsx::write_file_atomic(staging / ".complete", "");
    std::error_code ec;
    if (fs::exists(out_dir, ec)) fsx::remove_tree(out_dir);  // incomplete leftovers
    fsx::rename_replace(staging, out_dir);
    fsx::remove_tree(sandbox);

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    log::verbose(std::format("built {} in {:.1f}s", package.key(), secs));
    return out_dir;
}

}  // namespace cpm::install

namespace cpm::install {

bool install_prebuilt(const build::Project& project, const lock::Package& package, const fs::path& entry,
                      const std::string& url, const std::string& integrity, bool allow_http, bool offline) {
    const std::string abi = project.abi.dir();
    fs::path out_dir = entry / "lib" / abi;
    if (library_intact(out_dir)) return true;

    std::string what = std::format("{} for abi {}", entry_name(package), abi);
    FileLock lock = FileLock::acquire(project.home / "locks" / (Sha256::hex(what).substr(0, 16) + ".lock"), what);
    if (library_intact(out_dir)) return true;
    discard_damaged(out_dir, what);
    std::error_code ec;

    fs::path archive = cached_archive(project.home, url, integrity, allow_http, offline,
                                      std::format("{} (prebuilt, abi {})", package.key(), abi));
    fs::path staging = entry / "lib" / (abi + ".tmp-prebuilt");
    if (fs::exists(staging, ec)) fsx::remove_tree(staging);
    extract_archive(archive, staging, 0, package.key() + " (prebuilt)");

    // The binary must have been built for exactly this fingerprint (abi.md §3 step 5).
    bool matches = false;
    if (auto text = fsx::try_read_file(staging / "meta.json")) {
        try {
            json::Value meta = json::parse(*text, json::kStrict);
            const json::Value& abi_json = meta.at("abi");
            json::Value expected = json::Value::object();
            for (const auto& [k, v] : project.abi.inputs) expected.set(k, v);
            json::Value inputs = abi_json.at("inputs");
            // Compare independent of member order.
            bool same = inputs.is_object() && inputs.as_object().size() == expected.as_object().size();
            for (const auto& m : expected.as_object()) same = same && inputs.find(m.key) && *inputs.find(m.key) == m.value;
            matches = same && abi_json.at("hash").as_string() == project.abi.hash;
        } catch (const std::exception&) {
            matches = false;
        }
    }
    if (!matches) {
        fsx::remove_tree(staging);
        log::warn(std::format("the prebuilt binary of {} was built for a different toolchain; building from source",
                              package.key()));
        return false;
    }
    fsx::write_file_atomic(staging / ".complete", "");
    if (!library_intact(staging)) {
        // Installed anyway, it would fail the check on every later use and be installed again each time.
        fsx::remove_tree(staging);
        log::warn(std::format("the prebuilt binary of {} lacks files its meta.json lists; building from source",
                              package.key()));
        return false;
    }
    if (fs::exists(out_dir, ec)) fsx::remove_tree(out_dir);
    fsx::rename_replace(staging, out_dir);
    log::status("Using", std::format("prebuilt {} (abi {})", package.key(), abi));
    return true;
}

}  // namespace cpm::install
