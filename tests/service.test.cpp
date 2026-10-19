// install/service: the credentials store behind cpm login (docs/spec/registry.md §7.2) and the search behind cpm search
// (§7.5). The HTTP side is exercised end to end against a running registry service.
#include "core/fsx.hpp"
#include "install/fetch.hpp"
#include "install/registry.hpp"
#include "install/service.hpp"
#include "json/parse.hpp"
#include "support/check.hpp"
#include "support/registry_fixture.hpp"

using namespace cpm;
namespace fs = std::filesystem;

TEST(tokens_are_kept_per_registry) {
    check::TempDir home;
    namespace service = install::service;
    CHECK(!service::load_token(home.path(), "https://a.example/").has_value());
    service::save_token(home.path(), "https://a.example", "cpm_one");  // stored under the normalized base URL
    service::save_token(home.path(), "https://b.example/", "cpm_two");
    CHECK_EQ(service::load_token(home.path(), "https://a.example/").value_or(""), "cpm_one");
    CHECK_EQ(service::load_token(home.path(), "https://b.example").value_or(""), "cpm_two");
    service::save_token(home.path(), "https://a.example/", "cpm_three");
    CHECK_EQ(service::load_token(home.path(), "https://a.example/").value_or(""), "cpm_three");

    auto doc = json::parse(fsx::read_file(home.path() / "credentials.json"), json::kStrict);
    CHECK_EQ(doc.at("credentialsVersion").as_int(), 1);
    CHECK(service::remove_token(home.path(), "https://a.example/"));
    CHECK(!service::remove_token(home.path(), "https://a.example/"));
    CHECK(!service::load_token(home.path(), "https://a.example/").has_value());
    CHECK(service::load_token(home.path(), "https://b.example/").has_value());
#ifndef _WIN32
    auto perms = fs::status(home.path() / "credentials.json").permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

TEST(error_responses_carry_the_service_message) {
    namespace service = install::service;
    CHECK_EQ(service::expect({.status = 201, .body = R"({"name":"fmt"})"}).at("name").as_string(), "fmt");
    CHECK_THROWS(service::expect({.status = 409, .body = R"({"error":"version_exists","message":"fmt@1.0.0 is already published."})"}),
                 "fmt@1.0.0 is already published.");
    CHECK_THROWS(service::expect({.status = 502, .body = "<html>bad gateway</html>"}), "HTTP 502");
}

TEST(search_answers_are_parsed) {
    namespace service = install::service;
    auto body = json::parse(R"({"packages": [
        {"name": "nlohmann-json", "description": "JSON for Modern C++", "latest": "3.12.0", "type": "header-only", "license": "MIT"},
        {"name": "fmt", "latest": "12.2.0"},
        {"description": "no name, so not a package"},
        "not an object"
    ], "total": 41, "page": 1})", json::kStrict);
    auto r = service::parse_search(body);
    CHECK_EQ(r.packages.size(), 2u);
    if (r.packages.size() != 2) return;
    CHECK_EQ(r.packages[0].name, "nlohmann-json");
    CHECK_EQ(r.packages[0].description, "JSON for Modern C++");
    CHECK_EQ(r.packages[0].latest, "3.12.0");
    CHECK_EQ(r.packages[0].type, "header-only");
    CHECK_EQ(r.packages[0].license, "MIT");
    CHECK_EQ(r.packages[1].description, "");  // missing fields are empty
    CHECK_EQ(r.total, 41);
    // A total smaller than the page itself is not believed.
    CHECK_EQ(service::parse_search(json::parse(R"({"packages": [{"name": "a"}], "total": 0})", json::kStrict)).total, 1);
    CHECK(service::parse_search(json::parse(R"({"packages": []})", json::kStrict)).packages.empty());
    CHECK_THROWS(service::parse_search(json::parse(R"({"message": "oops"})", json::kStrict)), "no \"packages\" list");
}

TEST(search_queries_are_percent_encoded) {
    CHECK_EQ(install::url::encode("fmt"), "fmt");
    CHECK_EQ(install::url::encode("A-z_0.9~"), "A-z_0.9~");
    CHECK_EQ(install::url::encode("c++ json/x&y=1"), "c%2B%2B%20json%2Fx%26y%3D1");
    CHECK_EQ(install::url::encode("\xc3\xbc"), "%C3%BC");  // UTF-8 bytes one by one

    // A fake service whose API is plain http: CPM refuses before sending anything, and says which URL it built.
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");
    fixture::write(reg.root / "config.json", R"({ "apiVersion": 1, "api": "http://127.0.0.1:9/api/v1" })");
    install::Registry registry(reg.url(), false, t.path() / "home", install::Freshness::Online);
    CHECK_THROWS(install::service::search(registry, "c++ json", t.path() / "home"),
                 "http://127.0.0.1:9/api/v1/search?q=c%2B%2B%20json over plain http");
}

TEST(static_registries_cannot_be_searched) {
    check::TempDir t;
    fixture::Registry reg(t.path() / "registry");  // config.json without "api"
    reg.publish("fmt", "1.0.0", {{"include/fmt.hpp", "#pragma once\n"}}, fixture::entry("header-only"));
    install::Registry registry(reg.url(), false, t.path() / "home", install::Freshness::Online);
    CHECK_THROWS(install::service::search(registry, "fmt", t.path() / "home"), "is a static registry, which cannot be searched");
}
