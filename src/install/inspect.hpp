#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "install/registry.hpp"
#include "install/service.hpp"
#include "lock/lockfile.hpp"
#include "manifest/manifest.hpp"

// What `cpm info`, `cpm tree` and `cpm why` print (docs/spec/cli.md). Pure functions over data the commands load.
namespace cpm::install {

// `cpm info`: `shown` is one of `package`'s versions; `details` comes from a registry service (nullopt for a static
// registry); `install` is the command that adds the package.
std::string render_info(const RegistryPackage& package, const RegistryVersion& shown,
                        const std::optional<service::PackageDetails>& details, std::string_view install);

struct TreeOptions {
    std::optional<int> depth;  // levels below the project; nullopt shows every level
    bool prod = false;         // leave out devDependencies
};

// `cpm tree`: the project ("<name> <version>") on top, and each package of the lockfile under the packages that
// require it.
std::string render_tree(const lock::Lockfile& lock, std::string_view project, const TreeOptions& options);

// What a dependent's manifest asks for `name`: a range, or the git or path source (see `requirement`). nullopt when
// that manifest is not on this machine. `dependent` is null for the project.
using RequirementOf = std::function<std::optional<std::string>(const lock::Package* dependent, const std::string& name)>;

// `cpm why`: the package `name` on top, then the chains of packages that require it, up to the project. Throws
// cpm::Error when the lockfile has no such package.
std::string render_why(const lock::Lockfile& lock, std::string_view project, const std::string& name,
                       const RequirementOf& requirement_of);

// A manifest's dependency as `cpm why` shows it: "^12.2.0", "git <url>#<ref>" or "path <path>".
std::string requirement(const manifest::DependencySpec& spec);

}  // namespace cpm::install
