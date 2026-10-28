#pragma once

#include <string>
#include <string_view>

#include "core/platform.hpp"

// Each library package's export macro, <NAME>_API (docs/spec/manifest.md §4.1), which CPM passes as a definition.
namespace cpm::build {

// The macro's name: the package name in upper case, every character that isn't a letter or a digit as '_', then
// "_API". "cpm-json" -> "CPM_JSON_API".
std::string export_macro(std::string_view package);

// How a compile sees a package: as the shared library it is building, as the tests that link that library, or in any
// other way (a static or header-only library, a path dependency, a dependency of what is compiled).
enum class ExportUse { BuildingShared, LinkingShared, Plain };

// The definition of `package`'s macro for a compile on `os`: "CPM_JSON_API=__declspec(dllexport)",
// "CPM_JSON_API=__attribute__((visibility(\"default\")))" off Windows, or "CPM_JSON_API=" for Plain.
std::string export_definition(std::string_view package, ExportUse use, Os os);

}  // namespace cpm::build
