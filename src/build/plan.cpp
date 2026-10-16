#include "build/plan.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

namespace cpm::build {

namespace {

using manifest::TargetKind;

void append(std::vector<std::string>& to, const std::vector<std::string>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

// Definitions in order, without duplicates; the same name with two values is an error.
std::vector<std::string> merge_defines(const std::vector<std::vector<std::string>>& groups) {
    std::vector<std::string> out;
    std::map<std::string, std::string> seen;  // name -> full define
    for (const auto& group : groups) {
        for (const auto& d : group) {
            std::string name = d.substr(0, d.find('='));
            auto [it, inserted] = seen.emplace(name, d);
            if (inserted) {
                out.push_back(d);
            } else if (it->second != d) {
                throw Error(std::format("conflicting definitions \"{}\" and \"{}\"", it->second, d),
                            "check \"defines\", \"publicDefines\" and the profile in cpm.jsonc");
            }
        }
    }
    return out;
}

// System libraries without duplicates, keeping the last occurrence (build.md §4.3).
std::vector<std::string> dedupe_keep_last(const std::vector<std::string>& libs) {
    std::vector<std::string> out;
    for (auto it = libs.rbegin(); it != libs.rend(); ++it) {
        if (std::find(out.begin(), out.end(), *it) == out.end()) out.push_back(*it);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string test_name(const std::string& test_source) {
    return paths::to_utf8(paths::from_utf8(test_source).stem());  // "tests/json.test.cpp" -> "json.test"
}

}  // namespace

std::string object_path(const std::string& build_dir, const std::string& source, const std::string& ext) {
    std::string rel;
    for (const auto& part : paths::from_utf8(source)) {
        std::string s = paths::to_utf8(part);
        if (s == "/" || s.empty()) continue;
        if (s == "..") s = "__";
        std::replace(s.begin(), s.end(), ':', '_');
        if (!rel.empty()) rel += '/';
        rel += s;
    }
    return std::format("{}/obj/{}{}", build_dir, rel, ext);
}

BuildPlan make_plan(const manifest::Manifest& m, const manifest::Profile& profile, const toolchain::Toolchain& tc,
                    const std::string& std_flag, const toolchain::Driver& driver, const Layout& layout,
                    const std::vector<Dependency>& dependencies) {
    BuildPlan plan;
    plan.root = m.root;
    plan.build_dir = std::format("cpm_modules/.cache/{}", profile.name);
    plan.out_dir = std::format("build/{}", profile.name);
    plan.profile = profile.name;
    plan.package = m.name;
    plan.package_version = m.version.to_string();
    plan.kind = layout.kind;
    plan.toolchain = tc;
    plan.notes = layout.notes;

    manifest::PlatformOverrides platform = manifest::effective_platform(m, tc.os);
    bool pic = layout.kind == TargetKind::SharedLib;
    toolchain::ProfileFlags pf = driver.profile_flags(profile, std_flag, pic);
    append(plan.notes, pf.notes);

    plan.cxxflags = pf.compile;
    append(plan.cxxflags, platform.cxxflags);
    append(plan.cxxflags, profile.cxxflags);

    std::vector<std::string> assertion_defines;
    if (!profile.assertions) assertion_defines.push_back("NDEBUG");
    std::vector<std::string> dep_defines, dev_dep_defines;
    for (const auto& d : dependencies) append(d.dev ? dev_dep_defines : dep_defines, d.public_defines);
    std::vector<std::string> root_defines = merge_defines({assertion_defines, profile.defines, m.defines,
                                                           m.public_defines, platform.defines,
                                                           platform.public_defines, dep_defines});
    for (const auto& d : root_defines) plan.defines.push_back(driver.define_flag(d));
    for (const auto& d : merge_defines({root_defines, dev_dep_defines})) {
        if (std::find(root_defines.begin(), root_defines.end(), d) == root_defines.end())
            plan.test_defines.push_back(driver.define_flag(d));
    }

    plan.incs.push_back(driver.include_flag("src"));
    if (layout.include_dir_exists) plan.incs.push_back(driver.include_flag(m.include_dir));
    for (const auto& d : dependencies) {
        if (!d.include_dir.empty()) (d.dev ? plan.test_incs : plan.incs).push_back(driver.include_flag(d.include_dir));
    }
    plan.test_incs.push_back(driver.include_flag("tests"));

    plan.ldflags = pf.link;
    append(plan.ldflags, platform.ldflags);
    append(plan.ldflags, profile.ldflags);

    std::vector<std::string> system_libs = m.system_libs;
    append(system_libs, platform.system_libs);
    for (const auto& d : dependencies) append(system_libs, d.system_libs);
    for (const auto& lib : dedupe_keep_last(system_libs)) plan.libs.push_back(driver.system_lib_flag(lib));

    const std::string ext = driver.object_ext();

    // Dependencies: path packages compile here into archives; store libraries link via -L/-l. `main_*`
    // is what the package's own link uses, `test_*` adds the dev-only dependencies.
    std::vector<std::string> main_dep_inputs, main_dep_libs, main_dep_implicit;
    std::vector<std::string> test_dep_inputs, test_dep_libs, test_dep_implicit;
    for (const auto& d : dependencies) {
        std::vector<std::string> inputs, libs, implicit;
        if (d.sources) {
            ArchiveStep ar;
            ar.output = std::format("{}/lib/deps/{}", plan.build_dir, driver.static_lib_file(d.name));
            std::vector<std::string> incs, defines;
            for (const auto& dir : d.sources->include_dirs) incs.push_back(driver.include_flag(dir));
            for (const auto& def : merge_defines({assertion_defines, profile.defines, d.sources->defines}))
                defines.push_back(driver.define_flag(def));
            for (const auto& file : d.sources->files) {
                std::string obj = object_path(std::format("{}/obj/deps/{}", plan.build_dir, d.name), file, ext);
                std::string src = d.sources->root.empty() ? file : d.sources->root + "/" + file;
                plan.compiles.push_back({src, obj, CompileStep::Kind::Dependency, incs, defines});
                ar.members.push_back(obj);
            }
            inputs.push_back(ar.output);
            plan.dependency_archives.push_back(std::move(ar));
        } else if (!d.lib_dir.empty()) {
            libs = driver.lib_flags(d.lib_dir, d.link_name);
            implicit.push_back(d.lib_file);
        }
        if (!d.dev) {
            append(main_dep_inputs, inputs);
            append(main_dep_libs, libs);
            append(main_dep_implicit, implicit);
        }
        append(test_dep_inputs, inputs);
        append(test_dep_libs, libs);
        append(test_dep_implicit, implicit);
    }
    std::vector<std::string> core_objects;
    for (const auto& src : layout.sources) {
        std::string obj = object_path(plan.build_dir, src, ext);
        plan.compiles.push_back({src, obj, CompileStep::Kind::Core});
        core_objects.push_back(obj);
    }

    // What a test links against, besides its own object.
    std::vector<std::string> test_inputs;
    std::vector<std::string> test_extra_libs;
    std::vector<std::string> test_implicit;
    std::vector<std::string> test_ldflags;

    switch (layout.kind) {
        case TargetKind::Executable: {
            std::string entry_obj = object_path(plan.build_dir, *layout.entry, ext);
            plan.compiles.push_back({*layout.entry, entry_obj, CompileStep::Kind::Entry});
            LinkStep link;
            link.name = m.name;
            link.output = std::format("{}/{}", plan.out_dir, driver.exe_file(m.name));
            link.inputs.push_back(entry_obj);
            append(link.inputs, core_objects);
            append(link.inputs, main_dep_inputs);
            link.extra_libs = main_dep_libs;
            link.implicit = main_dep_implicit;
            plan.main_output = link.output;
            plan.main = std::move(link);
            test_inputs = core_objects;
            break;
        }
        case TargetKind::StaticLib: {
            ArchiveStep ar;
            ar.output = std::format("{}/{}", plan.out_dir, driver.static_lib_file(m.name));
            ar.members = core_objects;
            plan.main_output = ar.output;
            test_inputs.push_back(ar.output);
            plan.archive = std::move(ar);
            break;
        }
        case TargetKind::SharedLib: {
            LinkStep link;
            link.name = m.name;
            link.shared = true;
            link.output = std::format("{}/{}", plan.out_dir, driver.shared_lib_file(m.name));
            link.inputs = core_objects;
            append(link.inputs, main_dep_inputs);
            link.extra_libs = main_dep_libs;
            link.implicit = main_dep_implicit;
            std::optional<std::string> implib = driver.import_lib_file(m.name);
            std::string implib_path = implib ? std::format("{}/{}", plan.out_dir, *implib) : std::string();
            link.extra_ldflags = driver.shared_link_flags(implib_path);
            plan.main_output = link.output;
            if (implib) {
                // Declared as an output so Ninja creates its directory and knows who writes it.
                link.implicit_outputs.push_back(implib_path);
                // MinGW: tests link the import library; the DLL is found at run time via PATH (cpm test).
                test_extra_libs.push_back(implib_path);
                test_implicit.push_back(implib_path);
                test_implicit.push_back(link.output);
            } else {
                test_extra_libs.push_back(link.output);
                if (tc.os != Os::Windows) test_ldflags.push_back("-Wl,-rpath,$ORIGIN/..");
            }
            plan.main = std::move(link);
            break;
        }
        case TargetKind::HeaderOnly: break;
    }

    for (const auto& test_src : layout.tests) {
        std::string obj = object_path(plan.build_dir, test_src, ext);
        plan.compiles.push_back({test_src, obj, CompileStep::Kind::Test});
        LinkStep link;
        link.name = test_name(test_src);
        link.output = std::format("{}/tests/{}", plan.out_dir, driver.exe_file(link.name));
        link.inputs.push_back(obj);
        append(link.inputs, test_inputs);
        append(link.inputs, test_dep_inputs);
        link.implicit = test_implicit;
        append(link.implicit, test_dep_implicit);
        link.extra_libs = test_extra_libs;
        append(link.extra_libs, test_dep_libs);
        link.extra_ldflags = test_ldflags;
        plan.tests.push_back(std::move(link));
    }

    if (!dependencies.empty()) {
        plan.dependency_stamp = plan.build_dir + "/dependencies.stamp";
        std::string text = "# The dependencies this build was planned with; a change makes everything rebuild.\n";
        for (const auto& d : dependencies) {
            text += std::format("{}@{} {}{}\n", d.name, d.version, d.identity, d.dev ? " dev" : "");
        }
        plan.dependency_stamp_text = text;
    }

    std::sort(plan.compiles.begin(), plan.compiles.end(),
              [](const CompileStep& a, const CompileStep& b) { return a.source < b.source; });
    return plan;
}

}  // namespace cpm::build
