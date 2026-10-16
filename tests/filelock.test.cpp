#include "core/filelock.hpp"
#include "support/check.hpp"

using namespace cpm;

TEST(locks_are_exclusive) {
    check::TempDir dir;
    auto file = dir.path() / "locks" / "x.lock";
    {
        FileLock held = FileLock::acquire(file, "test");
        CHECK(!FileLock::try_acquire(file).has_value());
    }
    auto again = FileLock::try_acquire(file);  // released when `held` went away
    CHECK(again.has_value());
    FileLock moved = std::move(*again);
    CHECK(!FileLock::try_acquire(file).has_value());
}
