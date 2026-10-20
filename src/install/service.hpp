#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json/value.hpp"

namespace cpm::install {
class Registry;
}

// The client side of a registry service (docs/spec/registry.md §7): stored tokens, authenticated requests and search.
namespace cpm::install::service {

namespace fs = std::filesystem;

// $CPM_HOME/credentials.json maps registry base URLs to the tokens `cpm login` obtained (registry.md §7.2).
std::optional<std::string> load_token(const fs::path& home, const std::string& registry_base);
void save_token(const fs::path& home, const std::string& registry_base, const std::string& token);
bool remove_token(const fs::path& home, const std::string& registry_base);

struct Request {
    // Every member has a default so callers can use designated initializers for the ones they need.
    std::string method = "GET";
    std::string url{};                       // absolute
    std::optional<std::string> token{};      // sent as Authorization: Bearer
    std::optional<json::Value> json_body{};  // sent as application/json
    std::optional<fs::path> file_body{};     // sent as is, with `content_type`
    std::string content_type{};
    std::vector<std::pair<std::string, std::string>> headers{};
    bool allow_http = false;
};

struct Response {
    int status = 0;
    std::string body;
};

// Sends a request with curl. Everything, the token included, goes to curl as a config file on standard input,
// so nothing secret appears on a command line. Throws cpm::Error when the service cannot be reached.
Response send(const Request& request, const fs::path& home);

// The parsed JSON body of a response with one of the `ok` statuses; otherwise throws cpm::Error carrying the
// service's message (registry.md §7.1).
json::Value expect(const Response& response, std::initializer_list<int> ok = {200, 201});

struct SearchHit {
    std::string name, description, latest, type, license;
};

struct SearchResult {
    std::vector<SearchHit> packages;  // the first page, in the service's order
    std::int64_t total = 0;           // all matches, on every page
};

// `cpm search`: GET search?q=<query> on the registry's service (registry.md §7.5). Throws cpm::Error for a static
// registry, which has no search.
SearchResult search(Registry& registry, std::string_view query, const fs::path& home);

// The body of a search response; throws cpm::Error when it is not one.
SearchResult parse_search(const json::Value& body);

// What a registry service knows about a package beyond its index document (registry.md §7.5), for `cpm info`.
struct PackageDetails {
    // Every member has a default so callers can use designated initializers for the ones they need.
    std::string description{}, license{}, repository{}, homepage{};  // "" when unknown
    std::vector<std::string> owners{};
    std::optional<std::int64_t> downloads{};  // of every version
};

// GET packages/<name> on the registry's service. nullopt for a static registry, and when the service has no such
// package; throws cpm::Error when the service fails.
std::optional<PackageDetails> details(Registry& registry, std::string_view name, const fs::path& home);

// The body of a package details response; missing or mistyped fields are left empty.
PackageDetails parse_details(const json::Value& body);

}  // namespace cpm::install::service
