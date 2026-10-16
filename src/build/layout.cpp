#include "build/layout.hpp"

#include <algorithm>
#include <format>

#include "core/error.hpp"
#include "core/glob.hpp"
#include "core/paths.hpp"

namespace cpm::build {

namespace {

namespace fs = std::filesystem;
using manifest::TargetKind;

std::optional<std::string> find_entry(const fs::path& root, std::string_view stem) {
    for (const char* ext : {"cpp", "cc", "cxx"}) {
        std::string rel = std::format("src/{}.{}", stem, ext);
        std::error_code ec;
        if (fs::is_regular_file(root / paths::from_utf8(rel), ec)) return rel;
    }
    return std::nullopt;
}

std::string normalize(const std::string& rel) {
    return paths::to_utf8(paths::from_utf8(rel).lexically_normal());
}

}  // namespace

Layout infer_layout(const manifest::Manifest& m, const manifest::PlatformOverrides& platform) {
    Layout layout;
    const fs::path& root = m.root;
    std::error_code ec;
    layout.include_dir_exists = fs::is_directory(root / paths::from_utf8(m.include_dir), ec);

    std::vector<std::string> patterns = m.sources.empty() ? manifest::default_sources() : m.sources;
    patterns.insert(patterns.end(), platform.sources.begin(), platform.sources.end());
    std::vector<std::string> all_sources = glob::find(root, patterns);
    layout.tests = glob::find(root, {"tests/*.test.{cpp,cc,cxx}"});

    if (m.main) {
        std::string entry = normalize(*m.main);
        if (!fs::is_regular_file(root / paths::from_utf8(entry), ec)) {
            throw Error(std::format("\"main\" is \"{}\", but that file does not exist", *m.main),
                        "fix the path in cpm.jsonc (it is relative to the package root)");
        }
        layout.entry = entry;
        if (m.type) {
            layout.kind = *m.type;
        } else {
            std::string stem = paths::to_utf8(paths::from_utf8(entry).stem());
            if (stem == "main") layout.kind = TargetKind::Executable;
            else if (stem == "lib") layout.kind = TargetKind::StaticLib;
            else throw Error(std::format("cannot infer the target type from \"main\": \"{}\"", *m.main),
                             "add \"type\": \"executable\" (or static-lib, shared-lib) to cpm.jsonc");
        }
    } else if (m.type) {
        layout.kind = *m.type;
        if (layout.kind == TargetKind::Executable) {
            layout.entry = find_entry(root, "main");
            if (!layout.entry) {
                throw Error("this package is an executable but has no src/main.cpp",
                            "create src/main.cpp, or set \"main\" in cpm.jsonc");
            }
        } else if (layout.kind != TargetKind::HeaderOnly) {
            layout.entry = find_entry(root, "lib");
        }
    } else {
        auto main_file = find_entry(root, "main");
        auto lib_file = find_entry(root, "lib");
        if (main_file) {
            layout.kind = TargetKind::Executable;
            layout.entry = main_file;
            if (lib_file) {
                layout.notes.push_back(std::format("both {} and {} exist; building an executable", *main_file, *lib_file) +
                                       " (set \"type\" in cpm.jsonc to choose)");
            }
        } else if (lib_file) {
            layout.kind = TargetKind::StaticLib;
            layout.entry = lib_file;
        } else if (all_sources.empty() && layout.include_dir_exists) {
            layout.kind = TargetKind::HeaderOnly;
        } else {
            throw Error("cannot infer the target type of this package",
                        "add src/main.cpp (executable), src/lib.cpp (library), or set \"type\" in cpm.jsonc");
        }
    }

    // Tests are never part of the package's own sources.
    std::erase_if(all_sources, [&](const std::string& s) {
        return std::find(layout.tests.begin(), layout.tests.end(), s) != layout.tests.end();
    });

    switch (layout.kind) {
        case TargetKind::Executable:
            std::erase(all_sources, *layout.entry);
            layout.sources = std::move(all_sources);
            break;
        case TargetKind::StaticLib:
        case TargetKind::SharedLib:
            if (layout.entry && std::find(all_sources.begin(), all_sources.end(), *layout.entry) == all_sources.end()) {
                all_sources.push_back(*layout.entry);
                std::sort(all_sources.begin(), all_sources.end());
            }
            layout.sources = std::move(all_sources);
            if (layout.sources.empty()) {
                throw Error(std::format("this package is a {} but has no source files", manifest::to_string(layout.kind)),
                            "add src/lib.cpp, or use \"type\": \"header-only\"");
            }
            break;
        case TargetKind::HeaderOnly:
            layout.entry.reset();
            if (!all_sources.empty()) {
                layout.notes.push_back(std::format("header-only package: {} source file(s) under src/ are not compiled",
                                                   all_sources.size()));
            }
            break;
    }
    return layout;
}

}  // namespace cpm::build
