#include "build/exports.hpp"

#include <format>

namespace cpm::build {

std::string export_macro(std::string_view package) {
    std::string name;
    for (char c : package) {
        if (c >= 'a' && c <= 'z') name += static_cast<char>(c - 'a' + 'A');
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) name += c;
        else name += '_';
    }
    return name + "_API";
}

std::string export_definition(std::string_view package, ExportUse use, Os os) {
    std::string value;
    if (use != ExportUse::Plain) {
        if (os == Os::Windows) value = use == ExportUse::BuildingShared ? "__declspec(dllexport)" : "__declspec(dllimport)";
        else value = "__attribute__((visibility(\"default\")))";
    }
    return std::format("{}={}", export_macro(package), value);
}

}  // namespace cpm::build
