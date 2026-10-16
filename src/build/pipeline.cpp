#include "build/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "build/layout.hpp"
#include "build/ninja_gen.hpp"
#include "build/runner.hpp"
#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "manifest/manifest.hpp"
#include "toolchain/probe.hpp"

namespace cpm::build {

namespace {

namespace fs = std::filesystem;

const manifest::Profile& pick_profile(const manifest::Manifest& m, const std::string& name) {
    auto it = m.profiles.find(name);
    if (it != m.profiles.end()) return it->second;
    std::vector<std::string> names;
    for (const auto& [n, p] : m.profiles) names.push_back(n);
    throw Error(std::format("unknown profile \"{}\"", name), std::format("available: {}", str::join(names, ", ")));
}

// `ar rcs` never removes members, so an archive whose member list changed is deleted before Ninja runs
// (build.md §4.5). Member lists are kept in archives.json in the build directory.
void drop_stale_archives(const BuildPlan& plan) {
    std::vector<const ArchiveStep*> archives;
    if (plan.archive) archives.push_back(&*plan.archive);
    for (const auto& a : plan.dependency_archives) archives.push_back(&a);
    if (archives.empty()) return;
    fs::path state_file = plan.root / paths::from_utf8(plan.build_dir) / "archives.json";
    json::Value state = json::Value::object();
    if (auto text = fsx::try_read_file(state_file)) {
        try {
            state = json::parse(*text, json::kStrict);
            if (!state.is_object()) state = json::Value::object();
        } catch (const Error&) {
            state = json::Value::object();
        }
    }
    for (const ArchiveStep* archive : archives) {
        json::Value members = json::Value::array();
        for (const auto& m : archive->members) members.push_back(m);
        const json::Value* previous = state.find(archive->output);
        if (previous != nullptr && !(*previous == members)) {
            std::error_code ec;
            fs::remove(plan.root / paths::from_utf8(archive->output), ec);
            log::verbose(std::format("member list of {} changed; rebuilding it", archive->output));
        }
        state.set(archive->output, std::move(members));
    }
    fsx::write_if_changed(state_file, json::write(state, {.sort_keys = true}) + "\n");
}

std::string format_seconds(double s) { return s < 10 ? std::format("{:.2f}s", s) : std::format("{:.1f}s", s); }

void derive_from_manifest(Project& p, const std::string& profile_name) {
    p.profile = pick_profile(p.manifest, profile_name);
    p.std_flag = toolchain::std_flag(p.toolchain, p.manifest.std, p.home);
    manifest::PlatformOverrides platform = manifest::effective_platform(p.manifest, p.toolchain.os);
    std::vector<std::string> defines = p.profile.defines;
    for (const auto* list : {&p.manifest.defines, &p.manifest.public_defines, &platform.defines, &platform.public_defines})
        defines.insert(defines.end(), list->begin(), list->end());
    std::vector<std::string> cxxflags = p.profile.cxxflags;
    cxxflags.insert(cxxflags.end(), platform.cxxflags.begin(), platform.cxxflags.end());
    p.abi = toolchain::fingerprint(p.toolchain, *p.driver, p.profile, p.manifest.std, defines, cxxflags);
}

}  // namespace

Project load_project(const fs::path& cwd, const std::string& profile, const std::optional<std::string>& cxx,
                     const manifest::UserConfig& config) {
    auto root = paths::find_project_root(cwd);
    if (!root) {
        throw Error(std::format("no cpm.jsonc found in {} or any parent directory", paths::to_utf8(cwd)),
                    "run 'cpm init' to create a package");
    }
    Project p;
    p.root = *root;
    p.manifest = manifest::load(*root / "cpm.jsonc", cwd);
    for (const auto& w : p.manifest.warnings) log::diagnostic(w);
    pick_profile(p.manifest, profile);  // fail on an unknown profile before probing anything
    p.home = paths::cpm_home();
    p.toolchain = toolchain::find_toolchain({cxx, config.cxx, p.home});
    p.driver = toolchain::make_driver(p.toolchain);
    derive_from_manifest(p, profile);
    return p;
}

void reload_manifest(Project& p) {
    p.manifest = manifest::load(p.root / "cpm.jsonc", p.root);
    derive_from_manifest(p, p.profile.name);
}

BuildResult run_build(const BuildRequest& req, const manifest::UserConfig& config) {
    auto start = std::chrono::steady_clock::now();
    auto lap = [&](std::string_view stage) {
        if (log::level() < log::Level::Trace) return;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        log::trace(std::format("{:7.1f} ms  {}", ms, stage));
    };

    Project project = load_project(req.cwd, req.profile, req.cxx, config);
    const manifest::Manifest& m = project.manifest;
    const toolchain::Toolchain& tc = project.toolchain;
    const toolchain::Driver& driver = *project.driver;
    const fs::path& home = project.home;
    lap("manifest and toolchain ready");

    std::vector<Dependency> dependencies;
    if (req.dependencies) {
        dependencies = req.dependencies(project);
    } else if (!m.dependencies.empty() || !m.dev_dependencies.empty()) {
        throw Error("internal error: the build was started without its dependencies");
    }
    lap("dependencies ready");

    Layout layout = infer_layout(m, manifest::effective_platform(m, tc.os));
    BuildResult result;
    result.plan = make_plan(m, project.profile, tc, project.std_flag, driver, layout, dependencies);
    BuildPlan& plan = result.plan;
    if (req.require_executable && plan.kind != manifest::TargetKind::Executable) {
        throw Error(std::format("{} is a {}, so there is no executable to run", plan.package,
                                manifest::to_string(plan.kind)),
                    "run one of its scripts with 'cpm run <script>', or its tests with 'cpm test'");
    }
    for (const auto& note : plan.notes) log::warn(note);
    lap("layout and plan ready");

    // Which Ninja targets to build.
    std::vector<std::string> targets;
    switch (req.targets) {
        case BuildRequest::Targets::Main: targets = {"all"}; break;
        case BuildRequest::Targets::MainAndTests: targets = {"all", "tests"}; break;
        case BuildRequest::Targets::Tests:
            for (std::size_t i = 0; i < plan.tests.size(); ++i) {
                const LinkStep& t = plan.tests[i];
                bool wanted = req.test_filters.empty() ||
                              std::any_of(req.test_filters.begin(), req.test_filters.end(),
                                          [&](const std::string& f) { return t.name.contains(f); });
                if (wanted) {
                    result.selected_tests.push_back(i);
                    targets.push_back(t.name);
                }
            }
            if (plan.tests.empty()) {
                log::info("no tests found (tests are tests/*.test.cpp)");
                return result;
            }
            if (targets.empty()) {
                throw Error(std::format("no test matches {}", str::join(req.test_filters, ", ")),
                            std::format("available: {}", str::join(test_target_names(plan), ", ")));
            }
            break;
    }

    log::status("Compiling", std::format("{} v{} ({}, {})", plan.package, plan.package_version, plan.profile,
                                         tc.description()));

    fs::path build_dir = plan.root / paths::from_utf8(plan.build_dir);
    if (req.clean) {
        fsx::remove_tree(build_dir);
        fsx::remove_tree(plan.root / paths::from_utf8(plan.out_dir));
    }
    fsx::create_dirs(build_dir);
    drop_stale_archives(plan);
    if (!plan.dependency_stamp.empty()) {
        std::string text = plan.dependency_stamp_text + std::format("abi={}\n", project.abi.dir());
        fsx::write_if_changed(plan.root / paths::from_utf8(plan.dependency_stamp), text);
    }

    std::string build_file = plan.build_dir + "/build.ninja";
    bool changed = fsx::write_if_changed(plan.root / paths::from_utf8(build_file), generate_ninja(plan, driver));
    lap(changed ? "build.ninja written" : "build.ninja unchanged");

    NinjaRun run;
    run.ninja = find_ninja(config.ninja, home);
    run.root = plan.root;
    run.build_file = build_file;
    run.targets = targets;
    run.jobs = req.jobs;
    run.verbose = log::verbose();
    lap("ninja found");

    std::error_code ec;
    if (changed || !fs::exists(plan.root / "compile_commands.json", ec)) update_compile_commands(run);

    // Windows cannot overwrite a running executable, but it can rename one (build.md §6).
    fs::path renamed_self;
    fs::path main_output = plan.main_output.empty() ? fs::path() : plan.root / paths::from_utf8(plan.main_output);
    fs::path self = paths::current_exe();
    if (!main_output.empty() && paths::same_file(main_output, self) &&
        dry_run(run).find(plan.main_output) != std::string::npos) {
        fs::path old = self;
        old += ".old";
        fs::remove(old, ec);
        fsx::rename_replace(self, old);
        renamed_self = old;
        log::verbose(std::format("renamed the running executable to {}", paths::to_utf8(old)));
    }

    lap("starting ninja");
    result.exit_code = run_ninja(run);
    lap("ninja finished");
    if (!renamed_self.empty() && !fs::exists(main_output, ec)) fsx::rename_replace(renamed_self, main_output);

    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (result.exit_code != 0) {
        log::error(std::format("build failed ({}, {})", plan.profile, format_seconds(result.seconds)));
        result.exit_code = 1;
        return result;
    }
    std::string what = plan.main_output.empty() ? std::string("header-only, nothing to link") : plan.main_output;
    if (req.targets != BuildRequest::Targets::Main) {
        std::size_t n = req.targets == BuildRequest::Targets::Tests ? result.selected_tests.size() : plan.tests.size();
        what = std::format("{} test{}", n, n == 1 ? "" : "s");
    }
    log::status("Finished", std::format("{} in {} -> {}", plan.profile, format_seconds(result.seconds), what));
    return result;
}

}  // namespace cpm::build
