// The files `cpm init` creates. The C++ templates stick to C++11 so they build with every `std`.

#include <format>

#include "build/exports.hpp"
#include "init/init.hpp"
#include "json/write.hpp"

namespace cpm::init {

namespace {

using manifest::TargetKind;

std::string manifest_file(const Options& o) {
    std::string s;
    s += "// The package manifest: JSON with comments and trailing commas (JSONC).\n";
    s += "// Reference: docs/spec/manifest.md in the CPM repository.\n";
    s += "{\n";
    s += std::format("  \"name\": {},\n", json::quote(o.name));
    s += std::format("  \"version\": {},\n", json::quote(o.version));
    s += std::format("  \"description\": {},\n", json::quote(o.description));
    if (!o.license.empty()) s += std::format("  \"license\": {},\n", json::quote(o.license));
    s += "\n";
    s += "  // The C++ standard this package, and everything built for it, is compiled with.\n";
    s += std::format("  \"std\": {},\n", json::quote(o.std));
    s += "\n";
    s += "  // executable | static-lib | shared-lib | header-only. Without \"type\", CPM infers it:\n";
    s += "  // src/main.cpp makes an executable, src/lib.cpp a static library, only include/ a header-only one.\n";
    s += std::format("  \"type\": {},\n", json::quote(manifest::to_string(o.kind)));
    s += "\n";
    s += "  // Commands for `cpm run <name>`, e.g. \"lint\": \"clang-tidy src/*.cpp\".\n";
    s += "  \"scripts\": {},\n";
    s += "\n";
    s += "  // Packages this one uses, e.g. \"fmt\": \"^10.2.0\" (added by `cpm install fmt`).\n";
    s += "  \"dependencies\": {},\n";
    s += "  // Packages only the tests use (`cpm install -D <name>`).\n";
    s += "  \"devDependencies\": {}\n";
    s += "\n";
    s += "  // Build settings: `cpm build` uses \"debug\", `cpm build --release` uses \"release\". For example:\n";
    s += "  // \"profiles\": { \"release\": { \"lto\": true }, \"debug\": { \"sanitizers\": [\"address\"] } }\n";
    s += "}\n";
    return s;
}

std::string gitignore_file() {
    std::string s;
    for (const auto& line : gitignore_lines()) s += line + "\n";
    return s;
}

// --- executable --------------------------------------------------------------------------------

std::string exe_main(const std::string& id) {
    return std::format(R"(#include <iostream>

#include "greeting.hpp"

int main() {{
    std::cout << {}::greeting("world") << '\n';
    return 0;
}}
)",
                       id);
}

std::string exe_greeting_hpp(const std::string& id) {
    return std::format(R"(#pragma once

#include <string>

namespace {} {{

// Everything in src/ except main.cpp is compiled once and linked into both the executable and every
// test, so tests can call it directly.
std::string greeting(const std::string& name);

}}  // namespace {}
)",
                       id, id);
}

std::string exe_greeting_cpp(const std::string& id) {
    return std::format(R"(#include "greeting.hpp"

namespace {} {{

std::string greeting(const std::string& name) {{
    return "Hello, " + name + "!";
}}

}}  // namespace {}
)",
                       id, id);
}

std::string exe_test(const std::string& id) {
    return std::format(R"(// Each tests/*.test.cpp is its own executable, built and run by `cpm test`.
// Exit code 0 means the test passed.
#include <cstdio>

#include "greeting.hpp"

int main() {{
    if ({}::greeting("CPM") != "Hello, CPM!") {{
        std::fputs("unexpected greeting\n", stderr);
        return 1;
    }}
    return 0;
}}
)",
                       id);
}

// --- libraries -------------------------------------------------------------------------------

// `macro`: the package's export macro (manifest.md §4.1), which a static or shared library's functions are marked with.
std::string lib_header(const std::string& id, bool header_only, const std::string& macro = "") {
    if (header_only) {
        return std::format(R"(#pragma once

// Public headers live in include/; packages that depend on this one see them.
namespace {} {{

inline int add(int a, int b) {{
    return a + b;
}}

}}  // namespace {}
)",
                           id, id);
    }
    return std::format(R"(#pragma once

// Public headers live in include/; packages that depend on this one see them.
// {} marks what the library exports when it is built as a shared library. CPM defines it.
namespace {} {{

{} int add(int a, int b);

}}  // namespace {}
)",
                       macro, id, macro, id);
}

std::string lib_source(const std::string& name, const std::string& id) {
    return std::format(R"(#include <{}/{}.hpp>

namespace {} {{

int add(int a, int b) {{
    return a + b;
}}

}}  // namespace {}
)",
                       name, name, id, id);
}

std::string lib_test(const std::string& name, const std::string& id) {
    return std::format(R"(// Each tests/*.test.cpp is its own executable, built and run by `cpm test`.
// Exit code 0 means the test passed.
#include <cstdio>

#include <{}/{}.hpp>

int main() {{
    if ({}::add(2, 3) != 5) {{
        std::fputs("add(2, 3) != 5\n", stderr);
        return 1;
    }}
    return 0;
}}
)",
                       name, name, id);
}

}  // namespace

std::vector<File> render(const Options& o) {
    std::string id = identifier(o.name);
    std::vector<File> files = {{"cpm.jsonc", manifest_file(o)}};
    switch (o.kind) {
        case TargetKind::Executable:
            files.push_back({"src/main.cpp", exe_main(id)});
            files.push_back({"src/greeting.hpp", exe_greeting_hpp(id)});
            files.push_back({"src/greeting.cpp", exe_greeting_cpp(id)});
            files.push_back({"tests/greeting.test.cpp", exe_test(id)});
            break;
        case TargetKind::StaticLib:
        case TargetKind::SharedLib:
            files.push_back({std::format("include/{}/{}.hpp", o.name, o.name), lib_header(id, false, build::export_macro(o.name))});
            files.push_back({"src/lib.cpp", lib_source(o.name, id)});
            files.push_back({std::format("tests/{}.test.cpp", o.name), lib_test(o.name, id)});
            break;
        case TargetKind::HeaderOnly:
            files.push_back({std::format("include/{}/{}.hpp", o.name, o.name), lib_header(id, true)});
            files.push_back({std::format("tests/{}.test.cpp", o.name), lib_test(o.name, id)});
            break;
    }
    files.push_back({".gitignore", gitignore_file()});
    return files;
}

}  // namespace cpm::init
