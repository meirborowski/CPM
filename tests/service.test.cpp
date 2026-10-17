// install/service: the credentials store behind cpm login (docs/spec/registry.md §7.2). The HTTP side is exercised
// end to end against a running registry service.
#include "core/fsx.hpp"
#include "install/service.hpp"
#include "json/parse.hpp"
#include "support/check.hpp"

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
