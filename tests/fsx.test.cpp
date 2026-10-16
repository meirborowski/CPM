#include <fstream>

#include "core/fsx.hpp"
#include "core/proc.hpp"
#include "support/check.hpp"

using namespace cpm;
namespace fs = std::filesystem;

TEST(write_if_changed_only_writes_on_change) {
    check::TempDir dir;
    fs::path f = dir.path() / "a" / "b.txt";
    CHECK(fsx::write_if_changed(f, "one"));
    CHECK(!fsx::write_if_changed(f, "one"));
    CHECK(fsx::write_if_changed(f, "two"));
    CHECK_EQ(fsx::read_file(f), "two");
    CHECK(!fsx::try_read_file(dir.path() / "missing").has_value());
}

TEST(rename_replaces_existing_target) {
    check::TempDir dir;
    fsx::write_file_atomic(dir.path() / "from", "new");
    fsx::write_file_atomic(dir.path() / "to", "old");
    fsx::rename_replace(dir.path() / "from", dir.path() / "to");
    CHECK_EQ(fsx::read_file(dir.path() / "to"), "new");
    CHECK(!fs::exists(dir.path() / "from"));
}

// The safety rule of docs/spec/store.md §7: deleting a tree never follows a link into its target.
TEST(remove_tree_does_not_follow_links) {
    check::TempDir dir;
    fs::path target = dir.path() / "store";
    fs::create_directories(target / "inner");
    std::ofstream(target / "inner" / "precious.txt") << "keep me";

    fs::path tree = dir.path() / "cpm_modules";
    fs::create_directories(tree / "pkg");
    std::ofstream(tree / "pkg" / "file.txt") << "x";
    fs::path link = tree / "pkg" / "include";
#ifdef _WIN32
    // A directory junction, as CPM creates when symlinks need privileges.
    auto r = proc::capture({.argv = {"cmd", "/d", "/c", "mklink", "/J", link.string(), target.string()}});
    CHECK_EQ(r.exit_code, 0);
#else
    fs::create_directory_symlink(target, link);
#endif
    CHECK(fsx::is_link(link));
    CHECK(fs::exists(link / "inner" / "precious.txt"));

    fsx::remove_tree(tree);
    CHECK(!fs::exists(tree));
    CHECK(fs::exists(target / "inner" / "precious.txt"));
}

TEST(remove_tree_refuses_roots) {
    CHECK_THROWS(fsx::remove_tree(fs::current_path().root_path()), "refusing to delete");
    fsx::remove_tree(fs::current_path() / "definitely-missing-cpm-test-dir");  // missing is fine
}

TEST(links_directories_without_copying) {
    check::TempDir dir;
    fs::path target = dir.path() / "store" / "pkg";
    fs::create_directories(target / "include");
    std::ofstream(target / "include" / "a.hpp") << "// a";
    fs::path link = dir.path() / "cpm_modules" / "pkg" / "include";
    fsx::LinkKind kind = fsx::link_directory(target / "include", link);
    CHECK(kind != fsx::LinkKind::Copy);  // symlink, or a junction without privileges
    CHECK(fsx::is_link(link));
    CHECK(fs::exists(link / "a.hpp"));
    std::ofstream(target / "include" / "b.hpp") << "// b";
    CHECK(fs::exists(link / "b.hpp"));  // a link, so later files show up too
    fsx::remove_tree(dir.path() / "cpm_modules");
    CHECK(fs::exists(target / "include" / "b.hpp"));

    fsx::LinkKind copied = fsx::link_directory(target / "include", dir.path() / "copy", fsx::LinkKind::Copy);
    CHECK(copied == fsx::LinkKind::Copy);
    CHECK(!fsx::is_link(dir.path() / "copy"));
    CHECK(fs::exists(dir.path() / "copy" / "a.hpp"));
}

#ifdef _WIN32
TEST(junctions_work_without_privileges) {
    check::TempDir dir;
    fs::create_directories(dir.path() / "t");
    std::ofstream(dir.path() / "t" / "f.txt") << "x";
    CHECK(fsx::link_directory(dir.path() / "t", dir.path() / "j", fsx::LinkKind::Junction) == fsx::LinkKind::Junction);
    CHECK(fs::exists(dir.path() / "j" / "f.txt"));
    fsx::remove_tree(dir.path() / "j");
    CHECK(fs::exists(dir.path() / "t" / "f.txt"));
}
#endif
