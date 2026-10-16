#include "init/init.hpp"

#include <algorithm>
#include <iterator>
#include <format>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"
#include "semver/version.hpp"

namespace cpm::init {

using manifest::TargetKind;

std::string name_from_directory(std::string_view dir_name) {
    auto valid = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; };
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };

    std::string name;
    bool in_invalid_run = false;
    for (char raw : str::to_lower(dir_name)) {
        if (valid(raw)) {
            name += raw;
            in_invalid_run = false;
        } else if (!in_invalid_run) {
            name += '-';
            in_invalid_run = true;
        }
    }
    auto trim = [&] {
        while (!name.empty() && !alnum(name.front())) name.erase(name.begin());
        while (!name.empty() && !alnum(name.back())) name.pop_back();
    };
    trim();
    if (name.size() > 64) {
        name.resize(64);
        trim();
    }
    return manifest::valid_package_name(name) ? name : std::string();
}

std::string identifier(std::string_view name) {
    static constexpr std::string_view keywords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "std",
        "main", "import", "module", "final", "override"};
    std::string id(name);
    std::replace(id.begin(), id.end(), '.', '_');
    std::replace(id.begin(), id.end(), '-', '_');
    if (!id.empty() && id.front() >= '0' && id.front() <= '9') id.insert(id.begin(), '_');
    if (std::find(std::begin(keywords), std::end(keywords), id) != std::end(keywords)) id += '_';
    return id;
}

const std::vector<std::string>& gitignore_lines() {
    // "/build/" is anchored to the package root so that directories such as src/build/ stay tracked.
    static const std::vector<std::string> lines = {"cpm_modules/", "/build/", "compile_commands.json"};
    return lines;
}

Result write(const Options& o) {
    std::error_code ec;
    if (fs::exists(o.dir / "cpm.jsonc", ec) && !o.force) {
        throw Error(std::format("{} already has a cpm.jsonc", paths::to_utf8(o.dir)),
                    "use --force to replace it (other existing files are never overwritten)");
    }
    fsx::create_dirs(o.dir);

    Result r;
    for (const auto& f : render(o)) {
        fs::path target = o.dir / paths::from_utf8(f.path);
        bool exists = fs::exists(target, ec);
        if (f.path == ".gitignore" && exists) {
            std::string current = fsx::read_file(target);
            std::vector<std::string> present = str::split_lines(current);
            std::string addition;
            for (const auto& line : gitignore_lines()) {
                bool has = std::any_of(present.begin(), present.end(),
                                       [&](const std::string& p) { return str::trim(p) == line; });
                if (!has) addition += line + "\n";
            }
            if (!addition.empty()) {
                if (!current.empty() && !current.ends_with('\n')) current += '\n';
                fsx::write_file_atomic(target, current + addition);
                r.gitignore_updated = true;
            } else {
                r.skipped.push_back(f.path);
            }
            continue;
        }
        if (exists && !(f.path == "cpm.jsonc" && o.force)) {
            r.skipped.push_back(f.path);
            continue;
        }
        if (exists) r.overwrote_manifest = true;
        fsx::write_file_atomic(target, f.content);
        r.created.push_back(f.path);
    }
    return r;
}

namespace {

std::optional<TargetKind> parse_kind_answer(std::string_view answer) {
    std::string a = str::to_lower(answer);
    if (a == "exe" || a == "bin" || a == "app") return TargetKind::Executable;
    if (a == "lib" || a == "static") return TargetKind::StaticLib;
    if (a == "shared" || a == "dll" || a == "so") return TargetKind::SharedLib;
    if (a == "header" || a == "headers") return TargetKind::HeaderOnly;
    return manifest::parse_target_kind(a);
}

}  // namespace

void ask_questions(Options& o, const Fixed& fixed, const Ask& ask, const Say& say) {
    bool ended = false;  // end of input: keep the defaults for everything else
    // Asks until `accept` returns true for the answer (or the default, on an empty answer).
    auto question = [&](const std::string& label, const std::string& current, auto accept) {
        while (!ended) {
            std::string prompt = current.empty() ? std::format("{}: ", label) : std::format("{}: ({}) ", label, current);
            std::optional<std::string> answer = ask(prompt);
            if (!answer) {
                ended = true;
                return;
            }
            std::string value(str::trim(*answer));
            if (value.empty()) {
                if (accept(current, true)) return;
                continue;
            }
            if (accept(value, false)) return;
        }
    };

    if (!fixed.name) {
        question("package name", o.name, [&](const std::string& v, bool is_default) {
            if (is_default && v.empty()) {
                say("a package name is required");
                return false;
            }
            if (!manifest::valid_package_name(v)) {
                say(std::format("\"{}\" is not a valid package name: use lowercase letters, digits, '.', '_' or '-', "
                                "starting and ending with a letter or digit", v));
                return false;
            }
            o.name = v;
            return true;
        });
    }
    question("version", o.version, [&](const std::string& v, bool) {
        if (!semver::Version::parse(v)) {
            say(std::format("\"{}\" is not a SemVer version such as 0.1.0", v));
            return false;
        }
        o.version = v;
        return true;
    });
    question("description", o.description, [&](const std::string& v, bool) {
        o.description = v;
        return true;
    });
    if (!fixed.kind) {
        question("type (executable, static-lib, shared-lib, header-only)", std::string(manifest::to_string(o.kind)),
                 [&](const std::string& v, bool) {
                     auto kind = parse_kind_answer(v);
                     if (!kind) {
                         say(std::format("unknown type \"{}\"", v));
                         return false;
                     }
                     o.kind = *kind;
                     return true;
                 });
    }
    if (!fixed.std) {
        question("C++ standard", o.std, [&](const std::string& v, bool) {
            const auto& stds = manifest::known_standards();
            if (std::find(stds.begin(), stds.end(), v) == stds.end()) {
                say(std::format("unknown C++ standard \"{}\": use {}", v, str::join(stds, ", ")));
                return false;
            }
            o.std = v;
            return true;
        });
    }
    question("license", o.license, [&](const std::string& v, bool) {
        o.license = v;
        return true;
    });

    if (o.name.empty()) {
        throw UsageError("no package name given", "pass --name <name>, or answer the \"package name\" question");
    }
}

}  // namespace cpm::init
