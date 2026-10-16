#include "cli/args.hpp"
#include "core/error.hpp"
#include "support/check.hpp"

using namespace cpm::cli;

namespace {

const std::vector<OptionSpec> kGlobal = {
    {"verbose", 'v', false, "", ""},
    {"dir", 'C', true, "<path>", ""},
};

const std::vector<CommandSpec> kCommands = {
    {"build", {}, "", "", {{"release", 0, false, "", ""}, {"jobs", 'j', true, "<n>", ""}, {"profile", 0, true, "<p>", ""}}},
    {"install", {"i", "add"}, "", "", {{"save-dev", 'D', false, "", ""}}},
};

ParsedArgs parse(std::vector<std::string> argv) { return parse_args(argv, kGlobal, kCommands); }

}  // namespace

TEST(parses_commands_options_and_positionals) {
    auto a = parse({"-v", "build", "--release", "-j8", "extra", "--profile=fast"});
    CHECK(a.command != nullptr);
    CHECK_EQ(std::string(a.command->name), "build");
    CHECK(a.flag("verbose"));
    CHECK(a.flag("release"));
    CHECK_EQ(a.value("jobs").value_or(""), "8");
    CHECK_EQ(a.value("profile").value_or(""), "fast");
    CHECK_EQ(a.positionals, (std::vector<std::string>{"extra"}));
}

TEST(short_option_clusters_and_separate_values) {
    auto a = parse({"i", "-Dv", "fmt", "-C", "dir"});
    CHECK_EQ(std::string(a.command->name), "install");  // alias
    CHECK(a.flag("save-dev"));
    CHECK(a.flag("verbose"));
    CHECK_EQ(a.value("dir").value_or(""), "dir");
    CHECK_EQ(a.positionals, (std::vector<std::string>{"fmt"}));
    CHECK_EQ(parse({"-vv", "build"}).count("verbose"), 2);
}

TEST(passthrough_after_double_dash) {
    auto a = parse({"build", "--", "--release", "-x"});
    CHECK(a.has_passthrough);
    CHECK(!a.flag("release"));
    CHECK_EQ(a.passthrough, (std::vector<std::string>{"--release", "-x"}));
}

TEST(usage_errors) {
    CHECK_THROWS(parse({"frob"}), "unknown command 'frob'");
    CHECK_THROWS(parse({"build", "--bogus"}), "unknown option '--bogus' for 'cpm build'");
    CHECK_THROWS(parse({"--release", "build"}), "unknown option '--release'");  // command option before the command
    CHECK_THROWS(parse({"build", "-j"}), "needs a value");
    CHECK_THROWS(parse({"build", "--release=yes"}), "does not take a value");
    CHECK_THROWS(parse({"install", "-x"}), "unknown option '-x'");
}
