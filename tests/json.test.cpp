#include "json/parse.hpp"
#include "json/write.hpp"
#include "support/check.hpp"

using namespace cpm;

TEST(parses_scalars) {
    CHECK(json::parse("null").is_null());
    CHECK_EQ(json::parse("true").as_bool(), true);
    CHECK_EQ(json::parse("false").as_bool(), false);
    CHECK_EQ(json::parse("42").as_int(), 42);
    CHECK_EQ(json::parse("-7").as_int(), -7);
    CHECK_EQ(json::parse("1.5").as_double(), 1.5);
    CHECK_EQ(json::parse("2e3").as_double(), 2000.0);
    CHECK_EQ(json::parse("\"hi\"").as_string(), "hi");
    CHECK_EQ(json::parse("9223372036854775807").as_int(), INT64_MAX);
    CHECK(json::parse("9223372036854775808").type() == json::Type::Double);  // overflow -> double
    CHECK_EQ(json::parse("-0.125e-1").as_double(), -0.0125);
    CHECK_THROWS(json::parse("1e999"), "number out of range");
}

TEST(parses_string_escapes) {
    CHECK_EQ(json::parse(R"("a\"b\\c\/d\n\t")").as_string(), "a\"b\\c/d\n\t");
    CHECK_EQ(json::parse(R"("\u00e9")").as_string(), "\xC3\xA9");
    CHECK_EQ(json::parse(R"("\ud83d\ude00")").as_string(), "\xF0\x9F\x98\x80");  // surrogate pair
    CHECK_THROWS(json::parse(R"("\ud83d")"), "surrogate");
    CHECK_THROWS(json::parse(R"("\x")"), "invalid escape");
    CHECK_THROWS(json::parse("\"a\nb\""), "control characters");
}

TEST(keeps_object_order_and_spans) {
    std::string text = R"({"b": 1, "a": [true, null]})";
    json::Value v = json::parse(text);
    const auto& obj = v.as_object();
    CHECK_EQ(obj.size(), 2u);
    CHECK_EQ(obj[0].key, "b");
    CHECK_EQ(obj[1].key, "a");
    json::Span s = v.find("a")->span();
    CHECK_EQ(text.substr(s.begin, s.end - s.begin), "[true, null]");
    json::Span k = obj[1].key_span;
    CHECK_EQ(text.substr(k.begin, k.end - k.begin), "\"a\"");
}

TEST(accepts_jsonc) {
    std::string text = R"(// leading comment
{
  /* block */ "a": 1, // trailing
  "b": [1, 2,],
})";
    json::Value v = json::parse(text);
    CHECK_EQ(v.find("a")->as_int(), 1);
    CHECK_EQ(v.find("b")->as_array().size(), 2u);
    CHECK_THROWS(json::parse(text, json::kStrict), "comments are not allowed");
    CHECK_THROWS(json::parse("[1,]", json::kStrict), "trailing commas");
}

TEST(rejects_bad_documents) {
    CHECK_THROWS(json::parse(""), "empty");
    CHECK_THROWS(json::parse("{\"a\": 1, \"a\": 2}"), "duplicate key \"a\"");
    CHECK_THROWS(json::parse("{\"a\" 1}"), "expected ':'");
    CHECK_THROWS(json::parse("[1 2]"), "expected ',' or ']'");
    CHECK_THROWS(json::parse("{} x"), "unexpected content");
    CHECK_THROWS(json::parse("01"), "unexpected content");
    CHECK_THROWS(json::parse("/* open"), "unterminated block comment");
    CHECK_THROWS(json::parse("\"open"), "unterminated string");
    CHECK_THROWS(json::parse(std::string(300, '[') + std::string(300, ']')), "too deep");
}

TEST(skips_bom) {
    CHECK_EQ(json::parse("\xEF\xBB\xBF{\"a\":1}").find("a")->as_int(), 1);
}

TEST(reports_line_and_column) {
    std::string text = "{\n  \"a\": 1,\n  \"b\" 2\n}";
    try {
        json::parse(text);
        CHECK(false);
    } catch (const json::ParseError& e) {
        SourceLoc loc = json::locate(text, {e.offset(), e.offset() + 1}, "x.json");
        CHECK_EQ(loc.line, 3u);
        CHECK_EQ(loc.col, 7u);
        CHECK_EQ(loc.line_text, "  \"b\" 2");
    }
}

TEST(writes_lockfile_style) {
    json::Value v = json::Value::object();
    v.set("z", json::Value::object());
    v.set("a", json::Value(json::Array{json::Value(1), json::Value("x/y")}));
    v.set("m", json::Value::array());
    CHECK_EQ(json::write(v), "{\n  \"z\": {},\n  \"a\": [\n    1,\n    \"x/y\"\n  ],\n  \"m\": []\n}");
    CHECK_EQ(json::write(v, {.sort_keys = true}),
             "{\n  \"a\": [\n    1,\n    \"x/y\"\n  ],\n  \"m\": [],\n  \"z\": {}\n}");
    CHECK_EQ(json::write(v, {.compact = true, .sort_keys = true}), "{\"a\":[1,\"x/y\"],\"m\":[],\"z\":{}}");
}

TEST(escapes_minimally) {
    CHECK_EQ(json::quote("a\"b\\c\n\x01/\xC3\xA9"), "\"a\\\"b\\\\c\\n\\u0001/\xC3\xA9\"");
    CHECK_EQ(json::write(json::Value(1.0)), "1.0");
    CHECK_EQ(json::write(json::Value(0.25)), "0.25");
}

TEST(round_trips) {
    std::string text = "{\n  \"a\": [\n    true,\n    null,\n    -3\n  ],\n  \"b\": \"\\u0007\"\n}";
    CHECK_EQ(json::write(json::parse(text, json::kStrict)), text);
}

TEST(round_trips_the_spec_lockfile) {
    // docs/spec/lockfile.md §3: a canonical lockfile reads and writes back byte for byte.
    // Tests run from the project root.
    std::FILE* f = std::fopen("docs/spec/examples/hello/cpm-lock.json", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    // The top-level key order is fixed and every other level is sorted; this file's top level is already
    // in that order, so writing without re-sorting the top level must reproduce it.
    json::Value v = json::parse(text, json::kStrict);
    CHECK_EQ(json::write(v) + "\n", text);
}
