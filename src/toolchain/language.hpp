#pragma once

#include <string_view>

namespace cpm::toolchain {

// The language a translation unit is compiled as (docs/spec/manifest.md §5).
enum class Language { C, Cxx };

// `.c` sources are C and every other source is C++. Case matters: `.C` is C++, as GCC treats it.
inline Language language_of(std::string_view source) { return source.ends_with(".c") ? Language::C : Language::Cxx; }

}  // namespace cpm::toolchain
