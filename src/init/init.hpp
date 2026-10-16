#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "manifest/manifest.hpp"

namespace cpm::init {

namespace fs = std::filesystem;

// Everything `cpm init` needs to create a package (docs/spec/cli.md, cpm init).
struct Options {
    fs::path dir;  // target directory; created if missing
    std::string name;
    std::string version = "0.1.0";
    std::string std = "c++23";
    std::string description;
    std::string license;
    manifest::TargetKind kind = manifest::TargetKind::Executable;
    bool force = false;  // overwrite an existing cpm.jsonc
};

struct File {
    std::string path;  // relative, '/'-separated
    std::string content;
};

// A package name derived from a directory name: lowercased, each run of invalid characters replaced by
// '-', leading/trailing punctuation trimmed. Empty if no valid name results.
std::string name_from_directory(std::string_view dir_name);

// The package name as a C++ identifier for namespaces: '.' and '-' become '_', a leading digit gets a
// '_' prefix, and C++ keywords get a '_' suffix.
std::string identifier(std::string_view name);

// The files of a new package. Pure: does not touch the disk.
std::vector<File> render(const Options& options);

// The lines CPM wants in .gitignore.
const std::vector<std::string>& gitignore_lines();

struct Result {
    std::vector<std::string> created;  // relative paths
    std::vector<std::string> skipped;  // existed already, left alone
    bool overwrote_manifest = false;
    bool gitignore_updated = false;    // an existing .gitignore got missing lines appended
};

// Writes the package. Never overwrites an existing file, except cpm.jsonc when `force` is set.
// Throws cpm::Error (before writing anything) if cpm.jsonc exists and `force` is not set.
Result write(const Options& options);

// Interactive questions (npm-style "question: (default) "). `ask` shows the prompt and returns the
// answer, or nullopt at end of input (then the remaining defaults are taken). `say` reports an invalid
// answer before the question is asked again.
using Ask = std::function<std::optional<std::string>(const std::string& prompt)>;
using Say = std::function<void(const std::string& message)>;

struct Fixed {  // values given on the command line, which are not asked for
    bool name = false;
    bool kind = false;
    bool std = false;
};

void ask_questions(Options& options, const Fixed& fixed, const Ask& ask, const Say& say);

}  // namespace cpm::init
