#include "install/service.hpp"

#include <algorithm>
#include <format>
#include <random>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "install/fetch.hpp"
#include "install/registry.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cpm::install::service {

namespace {

fs::path credentials_file(const fs::path& home) { return home / "credentials.json"; }

json::Value load_credentials(const fs::path& home) {
    json::Value doc;
    if (auto text = fsx::try_read_file(credentials_file(home))) {
        try {
            doc = json::parse(*text, json::kStrict);
        } catch (const Error&) {
            doc = json::Value();
        }
    }
    if (!doc.is_object()) doc = json::Value::object();
    doc.set("credentialsVersion", 1);
    if (!doc.find("registries") || !doc.find("registries")->is_object()) doc.set("registries", json::Value::object());
    return doc;
}

void store_credentials(const fs::path& home, const json::Value& doc) {
#ifndef _WIN32
    // Only the owner may read tokens: create the file (and its temporary) with mode 0600.
    mode_t old = umask(077);
    try {
        fsx::write_file_atomic(credentials_file(home), json::write(doc) + "\n");
    } catch (...) {
        umask(old);
        throw;
    }
    umask(old);
    std::error_code ec;
    fs::permissions(credentials_file(home), fs::perms::owner_read | fs::perms::owner_write, ec);
#else
    fsx::write_file_atomic(credentials_file(home), json::write(doc) + "\n");
#endif
}

// A value in curl's config file syntax: double quotes, with backslash and double quote escaped.
std::string quoted(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n' || c == '\r') continue;  // a header or URL never spans lines
        out += c;
    }
    return out + "\"";
}

std::string random_suffix() {
    std::random_device rd;
    return std::format("{:08x}", rd());
}

}  // namespace

std::optional<std::string> load_token(const fs::path& home, const std::string& registry_base) {
    json::Value doc = load_credentials(home);
    const json::Value* entry = doc.find("registries")->find(url::as_base(registry_base));
    if (!entry) return std::nullopt;
    const json::Value* token = entry->find("token");
    if (!token || !token->is_string() || token->as_string().empty()) return std::nullopt;
    return token->as_string();
}

void save_token(const fs::path& home, const std::string& registry_base, const std::string& token) {
    json::Value doc = load_credentials(home);
    json::Value entry = json::Value::object();
    entry.set("token", token);
    doc.find("registries")->set(url::as_base(registry_base), std::move(entry));
    store_credentials(home, doc);
}

bool remove_token(const fs::path& home, const std::string& registry_base) {
    json::Value doc = load_credentials(home);
    json::Object& registries = doc.find("registries")->as_object();
    std::string key = url::as_base(registry_base);
    auto it = std::find_if(registries.begin(), registries.end(), [&](const json::Member& m) { return m.key == key; });
    if (it == registries.end()) return false;
    registries.erase(it);
    store_credentials(home, doc);
    return true;
}

Response send(const Request& request, const fs::path& home) {
    if (!url::is_http(request.url)) throw Error(std::format("unsupported URL '{}'", request.url));
    if (request.url.starts_with("http://") && !request.allow_http) {
        throw Error(std::format("refusing to talk to {} over plain http", request.url),
                    "use https, or set \"allowHttp\": true for this registry in $CPM_HOME/config.jsonc");
    }
    auto curl = proc::find_program("curl");
    if (!curl) throw Error("talking to a registry service needs curl, which was not found on PATH", "install curl (https://curl.se)");

    fs::path tmp = home / "tmp" / ("api-" + random_suffix());
    fsx::create_dirs(tmp);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            try {
                fsx::remove_tree(dir);
            } catch (...) {
            }
        }
    } cleanup{tmp};

    std::string config;
    auto option = [&](std::string_view name, std::string_view value) { config += std::format("{} = {}\n", name, quoted(value)); };
    config += "silent\nshow-error\n";
    option("proto", request.allow_http ? "=https,http" : "=https");
    option("request", request.method);
    option("url", request.url);
    option("output", paths::to_utf8(tmp / "response"));
    option("write-out", "%{http_code}");
    option("header", "Accept: application/json");
    option("header", "Expect:");  // no "100 Continue" round trip before uploads
    if (request.token) option("header", "Authorization: Bearer " + *request.token);
    for (const auto& [name, value] : request.headers) option("header", name + ": " + value);
    if (request.json_body) {
        fsx::write_file_atomic(tmp / "body.json", json::write(*request.json_body, {.compact = true}));
        option("header", "Content-Type: application/json");
        option("data-binary", "@" + paths::to_utf8(tmp / "body.json"));
    } else if (request.file_body) {
        option("header", "Content-Type: " + request.content_type);
        option("data-binary", "@" + paths::to_utf8(*request.file_body));
    }

    proc::Captured r = proc::capture({.argv = {paths::to_utf8(*curl), "--config", "-"}, .input = config});
    if (r.exit_code != 0) {
        throw Error(std::format("cannot reach {}: {}", request.url, str::trim(r.err)), "check the registry URL and your network access");
    }
    Response response;
    std::string code(str::trim(r.out));
    response.status = code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; }) ? std::stoi(code) : 0;
    response.body = fsx::try_read_file(tmp / "response").value_or("");
    return response;
}

json::Value expect(const Response& response, std::initializer_list<int> ok) {
    json::Value body;
    try {
        body = response.body.empty() ? json::Value() : json::parse(response.body, json::kStrict);
    } catch (const Error&) {
        body = json::Value();
    }
    if (std::find(ok.begin(), ok.end(), response.status) != ok.end()) return body;

    std::string message = std::format("the registry answered HTTP {}", response.status);
    if (const json::Value* m = body.find("message"); m && m->is_string()) message = m->as_string();
    if (response.status == 401) throw Error(message, "run cpm login");
    throw Error(message);
}

SearchResult parse_search(const json::Value& body) {
    const json::Value* packages = body.find("packages");
    if (!packages || !packages->is_array()) throw Error("the registry's search answer has no \"packages\" list");
    auto text = [](const json::Value& v, std::string_view key) {
        const json::Value* m = v.find(key);
        return m && m->is_string() ? m->as_string() : std::string();
    };
    SearchResult result;
    for (const auto& p : packages->as_array()) {
        if (!p.is_object() || text(p, "name").empty()) continue;
        result.packages.push_back({text(p, "name"), text(p, "description"), text(p, "latest"), text(p, "type"), text(p, "license")});
    }
    const json::Value* total = body.find("total");
    result.total = total && total->is_int() ? total->as_int() : 0;
    result.total = std::max<std::int64_t>(result.total, static_cast<std::int64_t>(result.packages.size()));
    return result;
}

SearchResult search(Registry& registry, std::string_view query, const fs::path& home) {
    const auto& api = registry.api();
    if (!api) {
        throw Error(std::format("{} is a static registry, which cannot be searched", registry.base()),
                    "search needs a registry service (a registry whose config.json names an \"api\"), such as the default "
                    "registry; static registries list their packages in index/");
    }
    Response r = send({.url = *api + "search?q=" + url::encode(query), .allow_http = registry.allow_http()}, home);
    return parse_search(expect(r, {200}));
}

PackageDetails parse_details(const json::Value& body) {
    auto text = [&](std::string_view key) {
        const json::Value* m = body.find(key);
        return m && m->is_string() ? m->as_string() : std::string();
    };
    PackageDetails d{text("description"), text("license"), text("repository"), text("homepage"), {}, std::nullopt};
    if (const json::Value* owners = body.find("owners"); owners && owners->is_array()) {
        for (const auto& o : owners->as_array()) {
            if (o.is_string()) d.owners.push_back(o.as_string());
        }
    }
    if (const json::Value* downloads = body.find("downloads"); downloads && downloads->is_object()) {
        if (const json::Value* total = downloads->find("total"); total && total->is_int()) d.downloads = total->as_int();
    }
    return d;
}

std::optional<PackageDetails> details(Registry& registry, std::string_view name, const fs::path& home) {
    const auto& api = registry.api();
    if (!api) return std::nullopt;
    Response r = send({.url = *api + "packages/" + url::encode(name), .allow_http = registry.allow_http()}, home);
    if (r.status == 404) return std::nullopt;
    return parse_details(expect(r, {200}));
}

}  // namespace cpm::install::service
