#pragma once

#include <string>

#include "build/plan.hpp"
#include "toolchain/driver.hpp"

namespace cpm::build {

// The complete build.ninja for a plan (docs/spec/build.md §4.5). Deterministic: the same plan always
// produces the same bytes.
std::string generate_ninja(const BuildPlan& plan, const toolchain::Driver& driver);

// Names of the phony targets for the tests, in plan order.
std::vector<std::string> test_target_names(const BuildPlan& plan);

}  // namespace cpm::build
