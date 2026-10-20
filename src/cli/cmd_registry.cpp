// cpm login / logout / whoami / yank / owner / search: accounts and packages on a registry service
// (docs/spec/registry.md §7, cli.md).
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/strings.hpp"
#include "core/term.hpp"
#include "install/registry.hpp"
#include "install/service.hpp"

namespace cpm::cli {

namespace {

namespace service = install::service;

// The registry a command talks to (--registry, or the default) and its API.
struct Target {
    manifest::UserConfig config;
    std::filesystem::path home;
    install::Registries registries;
    install::Registry& registry;
    std::string api;

    explicit Target(const ParsedArgs& args) :
        config(load_config()),
        home(paths::cpm_home()),
        registries(config, home, install::Freshness::Online),
        registry(registries.get(args.value("registry").value_or(""))) {
        const auto& a = registry.api();
        if (!a) {
            throw Error(std::format("{} is a static registry, so it has no accounts", registry.base()),
                        "publish to it with cpm publish --registry <file:// URL>, or with --out <dir> and an upload");
        }
        api = *a;
    }

    std::string token() const {
        auto t = service::load_token(home, registry.base());
        if (!t) throw Error(std::format("you are not signed in to {}", registry.base()), "run cpm login");
        return *t;
    }

    json::Value call(const std::string& method, const std::string& path, std::optional<std::string> token,
                     std::optional<json::Value> body = std::nullopt, std::initializer_list<int> ok = {200, 201}) const {
        service::Response r = service::send(
            {.method = method, .url = api + path, .token = std::move(token), .json_body = std::move(body), .allow_http = registry.allow_http()},
            home);
        return service::expect(r, ok);
    }
};

std::string string_of(const json::Value& v, std::string_view key) {
    const json::Value* m = v.find(key);
    return m && m->is_string() ? m->as_string() : std::string();
}

// Opens a URL in the user's browser, when there is a user to see it. Failures are ignored: the URL is printed too.
void open_browser(const std::string& url) {
    if (!url.starts_with("https://") || !term::is_terminal(2)) return;
#ifdef _WIN32
    std::vector<std::string> argv = {"rundll32", "url.dll,FileProtocolHandler", url};
#elif defined(__APPLE__)
    std::vector<std::string> argv = {"open", url};
#else
    std::vector<std::string> argv = {"xdg-open", url};
#endif
    if (!proc::find_program(argv[0])) return;
    try {
        proc::capture({.argv = argv});
    } catch (const Error&) {
    }
}

// GitHub's device flow, relayed by the registry (registry.md §7.2).
std::pair<std::string, std::string> device_login(const Target& t) {
    json::Value start = t.call("POST", "login/device", std::nullopt);
    std::string device = string_of(start, "deviceCode");
    std::string code = string_of(start, "userCode");
    std::string uri = string_of(start, "verificationUri");
    auto number = [&](std::string_view key, std::int64_t fallback) {
        const json::Value* v = start.find(key);
        return v && v->is_int() ? v->as_int() : fallback;
    };
    std::int64_t interval = std::max<std::int64_t>(1, number("interval", 5));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(number("expiresIn", 900));
    if (device.empty() || code.empty() || uri.empty()) throw Error("the registry did not start a sign-in");

    log::info(std::format("Sign in to {} with GitHub:", t.registry.base()));
    log::info(std::format("  1. open {}", uri));
    log::info(std::format("  2. enter the code {}", log::bold(code)));
    open_browser(uri);
    log::status("Waiting", "for you to approve the sign-in on GitHub (Ctrl-C to cancel)");

    json::Value body = json::Value::object();
    body.set("deviceCode", device);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        service::Response r = service::send(
            {.method = "POST", .url = t.api + "login/device/token", .json_body = body, .allow_http = t.registry.allow_http()}, t.home);
        if (r.status == 428) continue;
        if (r.status == 429) {
            interval += 5;
            continue;
        }
        json::Value done = service::expect(r, {200});
        return {string_of(done, "token"), string_of(done, "login")};
    }
    throw Error("the sign-in code expired", "run cpm login again");
}

// "<name>@<version>"
std::pair<std::string, std::string> name_at_version(const std::string& spec) {
    std::size_t at = spec.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == spec.size()) {
        throw UsageError(std::format("expected <name>@<version>, not '{}'", spec), "e.g. cpm yank fmt@12.2.0");
    }
    return {spec.substr(0, at), spec.substr(at + 1)};
}

}  // namespace

int cmd_login(const ParsedArgs& args, Context&) {
    if (!args.positionals.empty()) throw UsageError("cpm login takes no arguments");
    Target t(args);
    std::string token, login;
    if (args.flag("with-token")) {
        std::string line;
        std::getline(std::cin, line);
        token = std::string(str::trim(line));
        if (!token.starts_with("cpm_")) throw UsageError("--with-token reads a token (cpm_…) from standard input");
        login = string_of(t.call("GET", "whoami", token), "login");
    } else {
        std::tie(token, login) = device_login(t);
    }
    if (token.empty()) throw Error("the registry did not return a token");
    service::save_token(t.home, t.registry.base(), token);
    log::status("Signed in", std::format("to {} as {}", t.registry.base(), login));
    return 0;
}

int cmd_logout(const ParsedArgs& args, Context&) {
    Target t(args);
    auto token = service::load_token(t.home, t.registry.base());
    if (!token) {
        log::info(std::format("Not signed in to {}", t.registry.base()));
        return 0;
    }
    try {
        t.call("DELETE", "tokens/current", token);
    } catch (const Error& e) {
        log::warn(std::format("could not revoke the token on the registry: {}", e.what()), "it is removed from this machine anyway");
    }
    service::remove_token(t.home, t.registry.base());
    log::status("Signed out", std::format("of {}", t.registry.base()));
    return 0;
}

int cmd_whoami(const ParsedArgs& args, Context&) {
    Target t(args);
    json::Value me = t.call("GET", "whoami", t.token());
    log::out(string_of(me, "login") + "\n");
    return 0;
}

int cmd_yank(const ParsedArgs& args, Context&) {
    if (args.positionals.size() != 1) throw UsageError("cpm yank takes one <name>@<version>");
    auto [name, version] = name_at_version(args.positionals[0]);
    Target t(args);
    bool undo = args.flag("undo");
    t.call("POST", std::format("packages/{}/{}/{}", name, version, undo ? "unyank" : "yank"), t.token());
    log::status(undo ? "Unyanked" : "Yanked", std::format("{}@{} on {}", name, version, t.registry.base()));
    return 0;
}

int cmd_owner(const ParsedArgs& args, Context&) {
    const auto& p = args.positionals;
    std::string action = p.empty() ? "" : p[0];
    if (!((action == "list" && p.size() == 2) || ((action == "add" || action == "remove") && p.size() == 3))) {
        throw UsageError("usage: cpm owner list <name> | cpm owner add <name> <github-login> | cpm owner remove <name> <github-login>");
    }
    Target t(args);
    json::Value r;
    if (action == "list") r = t.call("GET", std::format("packages/{}/owners", p[1]), std::nullopt);
    else r = t.call(action == "add" ? "PUT" : "DELETE", std::format("packages/{}/owners/{}", p[1], p[2]), t.token());
    if (action != "list") log::status(action == "add" ? "Added" : "Removed", std::format("{} as an owner of {}", p[2], p[1]));
    if (const json::Value* owners = r.find("owners"); owners && owners->is_array()) {
        for (const auto& o : owners->as_array()) {
            if (o.is_string()) log::out(o.as_string() + "\n");
        }
    }
    return 0;
}

int cmd_search(const ParsedArgs& args, Context&) {
    if (args.positionals.empty()) throw UsageError("cpm search needs something to search for", "e.g. cpm search json");
    std::string query = str::join(args.positionals, " ");
    std::filesystem::path home = paths::cpm_home();
    install::Registries registries(load_config(), home, install::Freshness::Online);
    install::Registry& registry = registries.get(args.value("registry").value_or(""));
    service::SearchResult result = service::search(registry, query, home);
    if (result.packages.empty()) {
        log::info(std::format("No packages on {} match \"{}\"", registry.base(), query));
        return 0;
    }

    // Names right-aligned like the verbs of status lines, then the latest version and the description.
    std::size_t name_width = 12, version_width = 1;
    for (const auto& p : result.packages) {
        name_width = std::max(name_width, str::printable(p.name).size());
        version_width = std::max(version_width, str::printable(p.latest).size());
    }
    for (const auto& p : result.packages) {
        std::string latest = p.latest.empty() ? "-" : str::printable(p.latest);
        std::string line = std::format("{:>{}} {:<{}}  {}", str::printable(p.name), name_width, latest, version_width, str::printable(p.description));
        line.erase(line.find_last_not_of(' ') + 1);  // no description
        log::out(line + "\n");
    }
    if (result.total > static_cast<std::int64_t>(result.packages.size())) {
        log::info(std::format("{} of {} matches shown; refine the search to see others", result.packages.size(), result.total));
    }
    return 0;
}

}  // namespace cpm::cli
