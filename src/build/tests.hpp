#pragma once

#include <chrono>

#include "build/pipeline.hpp"

namespace cpm::build {

struct TestOptions {
    std::chrono::seconds timeout{300};
    bool show_output = false;  // also for passing tests
};

// Runs the tests selected by a successful build (docs/spec/build.md §7). Returns 0 if none failed.
int run_tests(const BuildResult& build, const TestOptions& options);

}  // namespace cpm::build
