#include "publish/publish.hpp"

#include <algorithm>
#include <format>

#include "build/layout.hpp"
#include "core/archive.hpp"
#include "core/error.hpp"
#include "core/filelock.hpp"
#include "core/fsx.hpp"
#include "core/glob.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "core/strings.hpp"
#include "install/fetch.hpp"
#include "install/registry.hpp"
#include "install/service.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"
#include "semver/version.hpp"

namespace cpm::publish {

namespace {

using manifest::DependencySpec;
using manifest::TargetKind;

std::string size_text(std::size_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024);
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024 * 1024));
}

TargetKind package_type(const manifest::Manifest& m) {
    TargetKind kind = build::infer_layout(m, {}).kind;
    if (kind == TargetKind::Executable) {
        throw Error(std::format("{} is an executable; only libraries can be published", m.name),
                    "registries hold static-lib, shared-lib and header-only packages");
    }
    return kind;
}

// Published packages may depend only on packages from the same registry (registry.md §3). devDependencies
// are never published, so they may come from anywhere.
void check_dependencies(const manifest::Manifest& m) {
    std::vector<std::string> problems;
    for (const auto& d : m.dependencies) {
        if (d.kind == DependencySpec::Kind::Git) problems.push_back(std::format("\"{}\" (git)", d.name));
        else if (d.kind == DependencySpec::Kind::Path) problems.push_back(std::format("\"{}\" (path)", d.name));
        else if (!d.registry.empty()) problems.push_back(std::format("\"{}\" (registry \"{}\")", d.name, d.registry));
    }
    if (!problems.empty()) {
        throw Error(std::format("{} cannot be published: it depends on {}", m.name, str::join(problems, ", ")),
                    "a published package may depend only on packages from the registry it is published to");
    }
}

void append(std::vector<std::string>& to, const std::vector<std::string>& from) { to.insert(to.end(), from.begin(), from.end()); }

// Sends the archive to a registry service (registry.md §7.3), which checks it again and updates its index.
void upload(const std::string& api, install::Registry& registry, const manifest::Manifest& m, const Package& pkg,
            const fs::path& home) {
    auto token = install::service::load_token(home, registry.base());
    if (!token) throw Error(std::format("you are not signed in to {}", registry.base()), "run cpm login");
    json::Value deps = json::Value::object();
    for (const auto& d : m.dependencies) deps.set(d.name, d.range);
    json::Value declared = json::Value::object();
    declared.set("type", std::string(manifest::to_string(pkg.type)));
    declared.set("std", m.std);
    declared.set("dependencies", std::move(deps));

    fs::path file = home / "tmp" / std::format("publish-{}-{}.tar.gz", pkg.name, pkg.version);
    fsx::write_file_atomic(file, pkg.archive);
    struct Remove {
        fs::path p;
        ~Remove() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } remove{file};
    install::service::Response r = install::service::send(
        {.method = "PUT",
         .url = std::format("{}packages/{}/{}", api, pkg.name, pkg.version),
         .token = token,
         .file_body = file,
         .content_type = "application/gzip",
         .headers = {{"X-Cpm-Integrity", pkg.integrity}, {"X-Cpm-Package", json::write(declared, {.compact = true})}},
         .allow_http = registry.allow_http()},
        home);
    install::service::expect(r, {201});
}

}  // namespace

std::vector<std::string> select_files(const manifest::Manifest& m) {
    // Always: the manifest, and the legal and readme files that should travel with every copy.
    std::vector<std::string> patterns = {"cpm.jsonc", "LICENSE*", "LICENCE*", "COPYING*", "NOTICE*", "README*"};
    if (!m.files.empty()) {
        append(patterns, m.files);
    } else {
        patterns.push_back("CHANGELOG*");
        if (!m.include_dir.empty() && m.include_dir != ".") patterns.push_back(m.include_dir + "/**");
        patterns.push_back("src/**");
        append(patterns, m.sources.empty() ? manifest::default_sources() : m.sources);
        for (const auto& [os, p] : m.platforms) append(patterns, p.sources);
    }
    for (const char* never : {"!build/**", "!cpm-lock.json", "!**/.DS_Store"}) patterns.push_back(never);
    return glob::find(m.root, patterns);
}

Package pack(const manifest::Manifest& m) {
    if (m.is_private) {
        throw Error(std::format("{} is private, so it cannot be published", m.name),
                    "remove \"private\": true from cpm.jsonc to publish it");
    }
    if (!m.version.build.empty()) {
        throw Error(std::format("version {} has build metadata, which registries do not allow", m.version.to_string()));
    }
    Package p;
    p.name = m.name;
    p.version = m.version.to_string();
    p.type = package_type(m);
    check_dependencies(m);
    p.files = select_files(m);

    std::string top = std::format("{}-{}", p.name, p.version);
    std::vector<archive::Entry> entries;
    for (const auto& f : p.files) {
        entries.push_back({.path = top + "/" + f, .data = fsx::read_file(m.root / paths::from_utf8(f))});
    }
    p.archive = archive::gzip(archive::tar(std::move(entries)));
    p.integrity = "sha256:" + Sha256::hex(p.archive);
    p.archive_path = std::format("packages/{}/{}.tar.gz", p.name, top);
    return p;
}

std::string add_version(const std::optional<std::string>& existing, const manifest::Manifest& m, const Package& pkg,
                        const std::string& registry_base) {
    json::Value doc;
    if (existing) {
        install::parse_index(*existing, pkg.name, registry_base);  // refuses to extend an invalid document
        doc = json::parse(*existing, json::kStrict);
        if (doc.at("versions").find(pkg.version)) {
            throw Error(std::format("{}@{} is already published, and published versions never change", pkg.name, pkg.version),
                        "raise \"version\" in cpm.jsonc");
        }
    } else {
        doc = json::Value::object();
        doc.set("name", pkg.name);
        doc.set("versions", json::Value::object());
    }

    json::Value source = json::Value::object();
    source.set("url", pkg.archive_path);
    source.set("integrity", pkg.integrity);
    source.set("stripComponents", 1);
    json::Value deps = json::Value::object();
    for (const auto& d : m.dependencies) deps.set(d.name, d.range);
    json::Value entry = json::Value::object();
    entry.set("source", std::move(source));
    entry.set("type", std::string(manifest::to_string(pkg.type)));
    entry.set("std", m.std);
    entry.set("dependencies", std::move(deps));

    json::Object& versions = doc.find("versions")->as_object();
    versions.push_back({pkg.version, std::move(entry), {}});
    std::stable_sort(versions.begin(), versions.end(), [](const json::Member& a, const json::Member& b) {
        return *semver::Version::parse(a.key) < *semver::Version::parse(b.key);  // valid: parse_index checked them
    });
    return json::write(doc) + "\n";
}

void publish(const fs::path& root, const Options& options, const manifest::UserConfig& config, const fs::path& home) {
    manifest::Manifest m = manifest::load(root / "cpm.jsonc", root);
    Package pkg = pack(m);
    log::status("Packed", std::format("{}@{} ({} files, {}, {})", pkg.name, pkg.version, pkg.files.size(),
                                      size_text(pkg.archive.size()), pkg.integrity));
    for (const auto& f : pkg.files) log::verbose("  " + f);

    install::Registries registries(config, home, install::Freshness::Online);
    install::Registry& registry = registries.get(options.registry);
    const std::string& base = registry.base();
    const std::string index_rel = "index/" + pkg.name + ".json";

    if (options.dry_run) {
        add_version(registry.index_text(pkg.name), m, pkg, base);  // the same checks as a real publish
        for (const auto& f : pkg.files) log::info("  " + f);
        log::status("Dry run", std::format("{}@{} would go to {}; nothing was written", pkg.name, pkg.version, base));
        return;
    }
    if (options.out) {
        std::string doc = add_version(registry.index_text(pkg.name), m, pkg, base);
        fsx::write_file_atomic(*options.out / paths::from_utf8(pkg.archive_path), pkg.archive);
        fsx::write_file_atomic(*options.out / paths::from_utf8(index_rel), doc);
        log::status("Wrote", std::format("{} and {} under {}; upload them to {}", pkg.archive_path, index_rel,
                                         paths::to_utf8(*options.out), base));
        return;
    }
    if (const auto& api = registry.api()) {
        upload(*api, registry, m, pkg, home);
        log::status("Published", std::format("{}@{} to {}", pkg.name, pkg.version, base));
        return;
    }
    if (!install::url::is_file(base)) {
        throw Error(std::format("CPM cannot write to {}", base),
                    "use --out <dir>, then upload the two files it writes to the registry's host");
    }

    // A local registry: write the archive, then the index, under the registry's lock so that concurrent
    // publishes cannot lose each other's index updates.
    fs::path dir = install::url::to_path(base);
    registry.index_text(pkg.name);  // checks config.json
    FileLock lock = FileLock::acquire(dir / ".cpm-publish.lock", "registry " + base);
    std::string doc = add_version(fsx::try_read_file(dir / paths::from_utf8(index_rel)), m, pkg, base);
    fsx::write_file_atomic(dir / paths::from_utf8(pkg.archive_path), pkg.archive);
    fsx::write_file_atomic(dir / paths::from_utf8(index_rel), doc);
    log::status("Published", std::format("{}@{} to {}", pkg.name, pkg.version, base));
}

}  // namespace cpm::publish
