#pragma once

// Builds a static file:// registry (docs/spec/registry.md) in a directory, for tests.

#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "install/fetch.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

namespace fixture {

namespace fs = std::filesystem;
using cpm::json::Value;

inline void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// tar -czf <archive> -C <dir> <entry>
inline void tar_gz(const fs::path& archive, const fs::path& dir, const std::string& entry) {
    fs::create_directories(archive.parent_path());
    auto r = cpm::proc::capture({.argv = {"tar", "-czf", cpm::paths::to_utf8(archive), "-C", cpm::paths::to_utf8(dir), entry}});
    if (r.exit_code != 0) throw std::runtime_error("tar failed: " + r.err);
}

struct Registry {
    fs::path root;

    explicit Registry(fs::path dir) : root(std::move(dir)) { write(root / "config.json", "{ \"apiVersion\": 1 }\n"); }

    std::string url() const { return cpm::install::url::from_path(root); }

    Value index(const std::string& name) const {
        if (auto text = cpm::fsx::try_read_file(root / "index" / (name + ".json"))) return cpm::json::parse(*text);
        Value doc = Value::object();
        doc.set("name", name);
        doc.set("versions", Value::object());
        return doc;
    }

    void save(const std::string& name, const Value& doc) const {
        write(root / "index" / (name + ".json"), cpm::json::write(doc) + "\n");
    }

    // Publishes files as <name>-<version>/... in packages/<name>/<name>-<version>.tar.gz (stripComponents 1).
    // `entry` holds the index fields besides "source" (type, std, dependencies, manifest, yanked).
    void publish(const std::string& name, const std::string& version, const std::map<std::string, std::string>& files,
                 Value entry) const {
        fs::path staging = root / ".staging";
        std::string top = name + "-" + version;
        cpm::fsx::remove_tree(staging);
        for (const auto& [path, text] : files) write(staging / top / path, text);
        fs::path archive = root / "packages" / name / (top + ".tar.gz");
        tar_gz(archive, staging, top);
        cpm::fsx::remove_tree(staging);
        Value source = Value::object();
        source.set("url", "packages/" + name + "/" + top + ".tar.gz");
        source.set("integrity", "sha256:" + cpm::install::sha256_file(archive));
        source.set("stripComponents", 1);
        entry.set("source", std::move(source));
        if (!entry.find("dependencies")) entry.set("dependencies", Value::object());
        Value doc = index(name);
        doc.find("versions")->set(version, std::move(entry));
        save(name, doc);
    }

    // Adds a prebuilt binary: the contents of a store lib/<abi16> directory.
    void add_binary(const std::string& name, const std::string& version, const fs::path& lib_dir) const {
        std::string abi = lib_dir.filename().string();
        fs::path archive = root / "binaries" / name / version / (abi + ".tar.gz");
        fs::path staging = root / ".staging-bin";
        cpm::fsx::remove_tree(staging);
        fs::create_directories(staging);
        for (const auto& e : fs::directory_iterator(lib_dir)) {
            if (e.path().filename() != ".complete") fs::copy(e.path(), staging / e.path().filename());
        }
        fs::create_directories(archive.parent_path());
        auto r = cpm::proc::capture({.argv = {"tar", "-czf", cpm::paths::to_utf8(archive), "-C", cpm::paths::to_utf8(staging), "."}});
        if (r.exit_code != 0) throw std::runtime_error("tar failed: " + r.err);
        cpm::fsx::remove_tree(staging);
        Value bin = Value::object();
        bin.set("url", "binaries/" + name + "/" + version + "/" + abi + ".tar.gz");
        bin.set("integrity", "sha256:" + cpm::install::sha256_file(archive));
        Value doc = index(name);
        Value& v = *doc.find("versions")->find(version);
        if (!v.find("binaries")) v.set("binaries", Value::object());
        v.find("binaries")->set(abi, std::move(bin));
        save(name, doc);
    }
};

// {"type": ..., "std": ..., "dependencies": {...}}
inline Value entry(const std::string& type, const std::string& std = "c++17",
                   std::map<std::string, std::string> deps = {}) {
    Value e = Value::object();
    e.set("type", type);
    e.set("std", std);
    Value d = Value::object();
    for (const auto& [n, r] : deps) d.set(n, r);
    e.set("dependencies", std::move(d));
    return e;
}

// A file:// registry whose index documents are written directly: resolution reads only the index, so the archives
// they name don't need to exist. Call save() once every version is added.
class Index {
public:
    explicit Index(fs::path dir) : reg_(std::move(dir)) {}

    Index& add(const std::string& name, const std::string& version, std::map<std::string, std::string> deps = {},
               bool yanked = false) {
        Value e = entry("header-only", "c++17", std::move(deps));
        Value source = Value::object();
        source.set("url", "packages/" + name + "-" + version + ".tar.gz");
        source.set("integrity", "sha256:" + std::string(64, '0'));
        e.set("source", std::move(source));
        if (yanked) e.set("yanked", true);
        auto [it, inserted] = docs_.try_emplace(name, Value::object());
        if (inserted) {
            it->second.set("name", name);
            it->second.set("versions", Value::object());
        }
        it->second.find("versions")->set(version, std::move(e));
        return *this;
    }

    void save() const {
        for (const auto& [name, doc] : docs_) reg_.save(name, doc);
    }

    std::string url() const { return reg_.url(); }
    std::string base() const { return cpm::install::url::as_base(reg_.url()); }

private:
    Registry reg_;
    std::map<std::string, Value> docs_;
};

}  // namespace fixture
