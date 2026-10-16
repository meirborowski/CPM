#include "manifest/config.hpp"

#include <array>
#include <format>

#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "json/parse.hpp"
#include "manifest/check.hpp"

namespace cpm::manifest {

UserConfig load_user_config(const std::filesystem::path& home) {
    UserConfig cfg;
    cfg.file = home / "config.jsonc";
    auto text = fsx::try_read_file(cfg.file);
    if (!text) return cfg;

    std::string display = paths::to_utf8(cfg.file);
    json::Value root;
    try {
        root = json::parse(*text);
    } catch (const json::ParseError& e) {
        Diagnostic d;
        d.message = e.what();
        d.loc = json::locate(*text, {e.offset(), e.offset() + 1}, display);
        throw DiagnosticsError({d});
    }

    Checker c(*text, display);
    if (!root.is_object()) {
        c.type_error(root, "the configuration", "a JSON object");
        throw DiagnosticsError(c.take_all());
    }
    static constexpr std::array<std::string_view, 6> keys = {"registries", "cxx", "ninja", "jobs", "color", "linkMode"};
    c.unknown_keys(root, keys, "config.jsonc");

    if (const json::Value* regs = c.object(root, "registries")) {
        for (const auto& m : regs->as_object()) {
            RegistryConfig r;
            if (m.value.is_string()) {
                r.url = m.value.as_string();
            } else if (m.value.is_object()) {
                static constexpr std::array<std::string_view, 2> rkeys = {"url", "allowHttp"};
                c.unknown_keys(m.value, rkeys, std::format("registry \"{}\"", m.key));
                r.url = c.string(m.value, "url").value_or("");
                r.allow_http = c.boolean(m.value, "allowHttp").value_or(false);
                if (r.url.empty()) c.error(m.value.span(), std::format("registry \"{}\" needs a \"url\"", m.key));
            } else {
                c.type_error(m.value, std::format("registry \"{}\"", m.key), "a URL string or an object");
                continue;
            }
            cfg.registries.emplace(m.key, std::move(r));
        }
    }
    cfg.cxx = c.string(root, "cxx");
    cfg.ninja = c.string(root, "ninja");
    if (const json::Value* jobs = root.find("jobs")) {
        if (jobs->is_int() && jobs->as_int() >= 0 && jobs->as_int() <= 4096) cfg.jobs = static_cast<int>(jobs->as_int());
        else c.error(jobs->span(), "\"jobs\" must be a non-negative integer");
    }
    if (auto color = c.string(root, "color")) {
        if (*color == "auto" || *color == "always" || *color == "never") cfg.color = color;
        else c.error(root.find("color")->span(), "\"color\" must be auto, always or never");
    }
    if (auto mode = c.string(root, "linkMode")) {
        if (*mode == "auto" || *mode == "symlink" || *mode == "junction" || *mode == "copy") cfg.link_mode = mode;
        else c.error(root.find("linkMode")->span(), "\"linkMode\" must be auto, symlink, junction or copy");
    }

    if (c.has_errors()) throw DiagnosticsError(c.take_all());
    cfg.warnings = c.take_warnings();
    return cfg;
}

}  // namespace cpm::manifest
