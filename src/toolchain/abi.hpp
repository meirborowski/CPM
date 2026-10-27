#pragma once

#include <string>
#include <utility>
#include <vector>

#include "manifest/profile.hpp"
#include "toolchain/driver.hpp"
#include "toolchain/toolchain.hpp"

namespace cpm::toolchain {

// The ABI fingerprint (docs/spec/abi.md): the toolchain and settings that decide binary compatibility.
struct AbiFingerprint {
    std::vector<std::pair<std::string, std::string>> inputs;  // sorted by key
    std::string hash;                                         // 64 lowercase hex

    std::string dir() const { return hash.substr(0, 16); }    // the store's lib/<abi16> directory name
    std::string canonical() const;                            // "key=value\n" lines that were hashed
};

// `defines`: the root package's definitions (profile, manifest and platform); only the ABI-affecting
// ones (abi.md §2) are used. `cxxflags`: raw flags, checked for -fno-exceptions / -fno-rtti (MSVC: /EHs-c- / /GR-).
AbiFingerprint fingerprint(const Toolchain& tc, const Driver& driver, const manifest::Profile& profile,
                           const std::string& std, const std::vector<std::string>& defines,
                           const std::vector<std::string>& cxxflags);

// Whether a definition (NAME or NAME=VALUE) is in the ABI-affecting list of abi.md §2.
bool is_abi_define(std::string_view define);

}  // namespace cpm::toolchain
