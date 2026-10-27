#include "build/tests.hpp"

#include <format>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"

namespace cpm::build {

namespace {

namespace fs = std::filesystem;

constexpr int kSkipped = 77;  // Automake convention

std::string seconds_text(double s) { return std::format("{:.2f}s", s); }

void show_output(const proc::Captured& r) {
    std::string text = r.out + r.err;
    if (text.empty()) return;
    if (!text.ends_with('\n')) text += '\n';
    log::err(text);
}

}  // namespace

int run_tests(const BuildResult& build, const TestOptions& options) {
    const BuildPlan& plan = build.plan;
    if (build.selected_tests.empty()) return 0;

    fs::path out_dir = plan.root / paths::from_utf8(plan.out_dir);
#ifdef _WIN32
    constexpr char path_sep = ';';
#else
    constexpr char path_sep = ':';
#endif
    // A shared library built by this package lives in build/<profile>/, and the compiler's runtime DLLs next to
    // the compiler (MSVC: in the directories vcvarsall.bat adds); Windows finds DLLs through PATH, so both go in front.
    std::string path = paths::to_native_utf8(out_dir);
    for (const auto& dir : toolchain::runtime_dirs(plan.toolchain)) path += path_sep + paths::to_native_utf8(dir);
    if (auto existing = env::get("PATH"); existing && !existing->empty()) path += path_sep + *existing;

    std::size_t passed = 0, failed = 0, skipped = 0;
    std::vector<std::string> failures;
    auto start = std::chrono::steady_clock::now();
    log::status("Running", std::format("{} test{}", build.selected_tests.size(), build.selected_tests.size() == 1 ? "" : "s"));

    for (std::size_t index : build.selected_tests) {
        const LinkStep& test = plan.tests[index];
        proc::Command cmd;
        cmd.argv = {paths::to_utf8(plan.root / paths::from_utf8(test.output))};
        cmd.cwd = plan.root;
        cmd.env_set = {{"CPM_TEST", "1"},
                       {"CPM_PROFILE", plan.profile},
                       {"CPM_PROJECT_ROOT", paths::to_utf8(plan.root)},
                       {"PATH", path}};

        auto t0 = std::chrono::steady_clock::now();
        proc::Captured r = proc::capture(cmd, std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        if (r.timed_out) {
            ++failed;
            failures.push_back(test.name);
            log::err(std::format("{} {} (timed out after {}s)\n", log::red(std::format("{:>12}", "FAIL")), test.name,
                                 options.timeout.count()));
            show_output(r);
        } else if (r.exit_code == 0) {
            ++passed;
            log::err(std::format("{} {} ({})\n", log::green(std::format("{:>12}", "PASS")), test.name, seconds_text(secs)));
            if (options.show_output) show_output(r);
        } else if (r.exit_code == kSkipped) {
            ++skipped;
            log::err(std::format("{} {}\n", log::yellow(std::format("{:>12}", "SKIP")), test.name));
            if (options.show_output) show_output(r);
        } else {
            ++failed;
            failures.push_back(test.name);
            log::err(std::format("{} {} (exit code {}, {})\n", log::red(std::format("{:>12}", "FAIL")), test.name,
                                 r.exit_code, seconds_text(secs)));
            show_output(r);
        }
    }

    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::string summary = std::format("{} passed", passed);
    if (skipped) summary += std::format(", {} skipped", skipped);
    if (failed) summary += std::format(", {} failed", failed);
    summary += std::format(" in {}", seconds_text(total));
    if (failed) {
        std::string names;
        for (const auto& f : failures) names += (names.empty() ? "" : ", ") + f;
        log::err(std::format("{} {} ({})\n", log::red(std::format("{:>12}", "Summary")), summary, names));
        return 1;
    }
    log::status("Summary", summary);
    return 0;
}

}  // namespace cpm::build
