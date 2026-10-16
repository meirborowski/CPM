#include "core/fsx.hpp"
#include "manifest/manifest.hpp"
#include "support/check.hpp"
#include "version.hpp"

using namespace cpm;

TEST(version_matches_cpm_jsonc) {
    // Tests run from the project root (docs/spec/build.md §7).
    manifest::Manifest m = manifest::load("cpm.jsonc", ".");
    CHECK_EQ(m.name, "cpm");
    CHECK_EQ(m.version.to_string(), std::string(cpm::version));
}
