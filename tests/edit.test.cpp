#include "json/edit.hpp"
#include "json/parse.hpp"
#include "support/check.hpp"

using namespace cpm;
using json::edit::remove_member;
using json::edit::set_member;

namespace {
const std::vector<std::string> kFmt = {"dependencies", "fmt"};
}

TEST(inserts_at_sorted_position_keeping_comments) {
    std::string text = R"({
  "name": "app", // the name
  "dependencies": {
    // logging
    "aaa": "1",
    // json
    "json": "^3"
  }
}
)";
    std::string out = set_member(text, kFmt, json::Value("^10.2.0"));
    CHECK_EQ(out, R"({
  "name": "app", // the name
  "dependencies": {
    // logging
    "aaa": "1",
    "fmt": "^10.2.0",
    // json
    "json": "^3"
  }
}
)");
}

TEST(appends_when_keys_are_unsorted) {
    std::string text = "{\n  \"dependencies\": {\n    \"z\": \"1\", // last\n    \"b\": \"2\" // trailing note\n  }\n}\n";
    std::string out = set_member(text, kFmt, json::Value("^10"));
    CHECK_EQ(out, "{\n  \"dependencies\": {\n    \"z\": \"1\", // last\n    \"b\": \"2\", // trailing note\n    \"fmt\": \"^10\"\n  }\n}\n");
}

TEST(keeps_trailing_comma_style) {
    std::string text = "{\n  \"dependencies\": {\n    \"a\": \"1\",\n  },\n}\n";
    std::string out = set_member(text, kFmt, json::Value("2"));
    CHECK_EQ(out, "{\n  \"dependencies\": {\n    \"a\": \"1\",\n    \"fmt\": \"2\",\n  },\n}\n");
}

TEST(fills_empty_objects_and_creates_missing_ones) {
    CHECK_EQ(set_member("{\n  \"dependencies\": {}\n}\n", kFmt, json::Value("1")),
             "{\n  \"dependencies\": {\n    \"fmt\": \"1\"\n  }\n}\n");
    CHECK_EQ(set_member("{\n  \"name\": \"x\"\n}\n", kFmt, json::Value("1")),
             "{\n  \"name\": \"x\",\n  \"dependencies\": {\n    \"fmt\": \"1\"\n  }\n}\n");
    // An empty object that only holds a comment keeps the comment.
    CHECK_EQ(set_member("{\n  \"dependencies\": {\n    // none yet\n  }\n}\n", kFmt, json::Value("1")),
             "{\n  \"dependencies\": {\n    // none yet\n    \"fmt\": \"1\"\n  }\n}\n");
}

TEST(updates_values_in_place) {
    std::string text = "{\n  \"dependencies\": { \"fmt\": \"^9\" /* old */ }\n}\n";
    CHECK_EQ(set_member(text, kFmt, json::Value("^10")), "{\n  \"dependencies\": { \"fmt\": \"^10\" /* old */ }\n}\n");
    CHECK_EQ(set_member(text, kFmt, json::Value("^9")), text);  // unchanged value: unchanged text
}

TEST(single_line_objects) {
    CHECK_EQ(set_member(R"({"dependencies": {"a": "1", "z": "2"}})", kFmt, json::Value("3")),
             R"({"dependencies": {"a": "1", "fmt": "3", "z": "2"}})");
    CHECK_EQ(set_member(R"({"dependencies": {"a": "1"}})", kFmt, json::Value("3")),
             R"({"dependencies": {"a": "1", "fmt": "3"}})");
}

TEST(object_values_are_written_on_several_lines) {
    json::Value spec = json::Value::object();
    spec.set("git", "https://x/y.git");
    spec.set("tag", "v1");
    std::string out = set_member("{\n  \"dependencies\": {}\n}\n", {"dependencies", "y"}, spec);
    CHECK_EQ(out, "{\n  \"dependencies\": {\n    \"y\": {\n      \"git\": \"https://x/y.git\",\n      \"tag\": \"v1\"\n    }\n  }\n}\n");
}

TEST(preserves_crlf) {
    std::string out = set_member("{\r\n  \"dependencies\": {\r\n    \"a\": \"1\"\r\n  }\r\n}\r\n", kFmt, json::Value("2"));
    CHECK_EQ(out, "{\r\n  \"dependencies\": {\r\n    \"a\": \"1\",\r\n    \"fmt\": \"2\"\r\n  }\r\n}\r\n");
}

TEST(removes_members) {
    std::string text = "{\n  \"dependencies\": {\n    \"a\": \"1\",\n    \"fmt\": \"2\", // why\n    \"z\": \"3\"\n  }\n}\n";
    CHECK_EQ(remove_member(text, kFmt), "{\n  \"dependencies\": {\n    \"a\": \"1\",\n    \"z\": \"3\"\n  }\n}\n");
    // Removing the last member also removes the separator before it.
    CHECK_EQ(remove_member("{\n  \"dependencies\": {\n    \"a\": \"1\",\n    \"fmt\": \"2\"\n  }\n}\n", kFmt),
             "{\n  \"dependencies\": {\n    \"a\": \"1\"\n  }\n}\n");
    // ...unless the file uses trailing commas.
    CHECK_EQ(remove_member("{\n  \"dependencies\": {\n    \"a\": \"1\",\n    \"fmt\": \"2\",\n  }\n}\n", kFmt),
             "{\n  \"dependencies\": {\n    \"a\": \"1\",\n  }\n}\n");
    CHECK_EQ(remove_member("{\n  \"dependencies\": {\n    \"fmt\": \"2\"\n  }\n}\n", kFmt), "{\n  \"dependencies\": {}\n}\n");
    CHECK_EQ(remove_member(R"({"dependencies": {"a": "1", "fmt": "2", "z": "3"}})", kFmt),
             R"({"dependencies": {"a": "1", "z": "3"}})");
    CHECK_EQ(remove_member(R"({"dependencies": {"a": "1", "fmt": "2"}})", kFmt), R"({"dependencies": {"a": "1"}})");
    CHECK_EQ(remove_member(text, {"dependencies", "missing"}), text);
}

TEST(round_trip_with_the_init_template_style) {
    std::string text = R"(// The package manifest
{
  "name": "app",
  "version": "0.1.0",

  // Packages this one uses
  "dependencies": {},
  // Packages only the tests use
  "devDependencies": {}

  // "profiles": { ... }
}
)";
    std::string added = set_member(text, kFmt, json::Value("file:../fmt"));
    CHECK(added.contains("  \"dependencies\": {\n    \"fmt\": \"file:../fmt\"\n  },\n"));
    CHECK(added.contains("// Packages only the tests use"));
    CHECK_EQ(remove_member(added, kFmt), text);
}
