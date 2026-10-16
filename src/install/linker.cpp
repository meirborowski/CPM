#include "install/linker.hpp"

#include <format>
#include <map>

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

namespace cpm::install {

namespace {

fs::path state_file(const fs::path& root) { return root / "cpm_modules" / ".cpm-modules.json"; }

json::Value read_state(const fs::path& root) {
    if (auto text = fsx::try_read_file(state_file(root))) {
        try {
            json::Value v = json::parse(*text, json::kStrict);
            if (v.is_object()) return v;
        } catch (const Error&) {
        }
    }
    return json::Value::object();
}

std::string str_or_empty(const json::Value* v) { return v && v->is_string() ? v->as_string() : std::string(); }

}  // namespace

bool modules_current(const fs::path& root, const std::string& lock_hash, const std::string& abi) {
    json::Value state = read_state(root);
    return str_or_empty(state.find("lockHash")) == lock_hash && str_or_empty(state.find("abi")) == abi;
}

void link_modules(const fs::path& root, const std::vector<LinkTarget>& targets, const std::string& lock_hash,
                  const std::string& abi, std::optional<fsx::LinkKind> mode) {
    fs::path modules = root / "cpm_modules";
    json::Value old_state = read_state(root);
    const json::Value* old_packages = old_state.find("packages");

    std::map<std::string, const LinkTarget*> wanted;
    for (const auto& t : targets) wanted[t.name] = &t;

    // Remove packages that are no longer installed (never following their links).
    for (const auto& e : fsx::list_dir(modules)) {
        if (e.name.starts_with('.')) continue;
        if (!wanted.contains(e.name)) fsx::remove_tree(modules / paths::from_utf8(e.name));
    }

    std::string used_mode;
    json::Value packages = json::Value::object();
    for (const auto& [name, t] : wanted) {
        std::string include = t->include ? paths::to_utf8(*t->include) : std::string();
        std::string lib = t->lib ? paths::to_utf8(*t->lib) : std::string();
        fs::path dir = modules / paths::from_utf8(name);

        const json::Value* before = old_packages ? old_packages->find(name) : nullptr;
        std::error_code ec;
        bool unchanged = before && str_or_empty(before->find("include")) == include &&
                         str_or_empty(before->find("lib")) == lib &&
                         (include.empty() || fs::exists(dir / "include", ec)) && (lib.empty() || fs::exists(dir / "lib", ec));
        if (!unchanged) {
            fsx::remove_tree(dir);
            fsx::create_dirs(dir);
            if (t->include) used_mode = std::string(fsx::to_string(fsx::link_directory(*t->include, dir / "include", mode)));
            if (t->lib) used_mode = std::string(fsx::to_string(fsx::link_directory(*t->lib, dir / "lib", mode)));
            log::verbose(std::format("linked cpm_modules/{}", name));
        }
        json::Value entry = json::Value::object();
        if (!include.empty()) entry.set("include", include);
        if (!lib.empty()) entry.set("lib", lib);
        entry.set("version", t->version);
        packages.set(name, std::move(entry));
    }
    if (used_mode.empty()) used_mode = str_or_empty(old_state.find("linkMode"));

    json::Value state = json::Value::object();
    state.set("cpmModulesVersion", 1);
    state.set("lockHash", lock_hash);
    state.set("abi", abi);
    if (!used_mode.empty()) state.set("linkMode", used_mode);
    state.set("packages", std::move(packages));
    fsx::write_if_changed(state_file(root), json::write(state) + "\n");
}

}  // namespace cpm::install
