# Architecture

## 1. Goals

1. **Seconds to first run.** `cpm init -y && cpm run` works with no build scripts. Adding a library is one command.
2. **Convention over configuration.** Targets, include paths and link lines are inferred from the directory layout and the lockfile. A two-field manifest (`name`, `version`) is a valid project.
3. **Never compile the same thing twice.** Sources and compiled artifacts live in one global store, keyed by `name@version` and by ABI fingerprint, and are shared by every project on the machine.
4. **Deterministic.** The same manifest, lockfile, toolchain and registry snapshot give the same resolution, the same `build.ninja`, and a byte-identical lockfile.
5. **Fast no-op.** A no-op `cpm build` SHOULD finish in under 150 ms, most of which is Ninja's own stat pass.

**Non-goals (MVP):** C sources, modules (`import`), cross-compilation, workspaces, a hosted registry service, IDE project generation (beyond `compile_commands.json`).

## 2. The four pillars

| Pillar | Summary | Spec |
|---|---|---|
| Global store | `$CPM_HOME/store/<name>@<version>/` holds extracted sources plus `lib/<abi16>/` artifacts. Projects link to it from `cpm_modules/` with symlinks or junctions. | [store.md](store.md) |
| ABI fingerprint + hybrid caching | SHA-256 over OS, arch, compiler and version, stdlib ABI, CRT, C++ standard, runtime, PIC and ABI-affecting flags. On install: prebuilt binary for this ABI if the registry has one, else build from source in a sandbox and cache. | [abi.md](abi.md), [registry.md](registry.md) |
| Flat resolution + ODR protection | One version per package name across the whole graph, recorded in `cpm-lock.json`. Diamond conflicts are errors; symbol isolation is an opt-in escape hatch (*post-MVP*). | [dependencies.md](dependencies.md), [lockfile.md](lockfile.md) |
| Direct Ninja generation | Toolchain drivers translate abstract profiles to compiler flags. CPM writes `build.ninja` itself and invokes Ninja. A Legacy Bridge wraps CMake/Meson packages (*post-MVP*). | [build.md](build.md) |

## 3. Module layering

CPM is a single executable. Source lives in `src/<module>/`, namespace `cpm::<module>`. The small utilities in `core/` each have their own namespace instead (`cpm::fsx`, `cpm::proc`, `cpm::log`, …), and the error types live directly in `cpm`. Headers are internal (`src/` is on the include path). A module may include only modules in the same or a lower layer, and modules in the same layer may not form include cycles.

```
 Layer 4  CLI         cli/
 Layer 3  Services    install/   build/   toolchain/   init/   run/   publish/
 Layer 2  Model       manifest/  lock/
 Layer 1  Foundation  json/  semver/  core/
```

| Module | Files (indicative) | Responsibility |
|---|---|---|
| `cli/` | `args`, `commands`, `cmd_init`, `cmd_install` (also `remove` and `update`), `cmd_build`, `cmd_run`, `cmd_test`, `cmd_publish`, `cmd_registry` (`login`, `logout`, `whoami`, `yank`, `owner`) | argv parsing (subcommands, short/long flags, `--` passthrough), help text, dispatch. Commands only orchestrate services. They hold no business logic. |
| `install/` | `installer`, `resolver`, `sources`, `registry`, `service`, `fetch`, `git`, `store`, `dep_builder`, `linker` | Install pipeline: resolve → fetch → ensure binaries → link → write lock. `Sources` turns specs into package metadata (path, git, registry index). Global store access and locking, sandboxed dependency builds, `cpm_modules/` linking. `service` is the client of registry services: stored tokens and authenticated API requests (registry.md §7). `build/` never includes `install/`: a build receives its dependencies through a callback (`BuildRequest::dependencies`) that the CLI fills with `install::ensure`. |
| `init/` | `init`, `templates` | `cpm init`: name derivation, C++ identifiers, file templates, non-destructive writing, interactive questions (input injected, so they are testable). |
| `run/` | `run` | `cpm run`: shell commands for scripts, the command for the built program, and the `CPM_*` environment. |
| `publish/` | `publish` | `cpm publish`: file selection, the checks before packing, deterministic source archives, adding a version to a registry index ([registry.md §6](registry.md#6-publishing)). |
| `build/` | `layout`, `plan`, `ninja_writer`, `ninja_gen`, `runner`, `dep_builder`, `tests` | Convention inference, `BuildPlan` construction, Ninja syntax and escaping, `build.ninja` generation, Ninja invocation, sandboxed dependency builds, test discovery and execution. |
| `toolchain/` | `probe`, `driver`, `gcc_driver`, `msvc_driver` (*post-MVP*), `abi` | Compiler discovery and cached probing, the `Driver` interface that maps profiles to flags and rules, ABI fingerprinting. |
| `manifest/` | `manifest`, `dep_spec`, `profile`, `validate` | `cpm.jsonc` model, overlays, profile defaults and inheritance, validation with `file:line:col` diagnostics. |
| `lock/` | `lockfile` | `cpm-lock.json` model, deterministic read and write, freshness check. |
| `json/` | `value`, `parse`, `write`, `edit` | Order-preserving JSON value with source spans, JSONC parser, deterministic writer, surgical comment-preserving text edits. |
| `semver/` | `version`, `range` | SemVer 2.0 versions. npm-style ranges as unions of disjoint intervals. |
| `core/` | `error`, `log`, `proc`, `fsx`, `paths`, `sha256`, `net`, `archive`, `glob`, `term` | Errors and diagnostics, logging and colour, subprocesses, filesystem (atomic writes, links, safe deletion, locks), CPM home paths, hashing, downloads (`curl`), writing deterministic `.tar.gz` archives (tar and DEFLATE implemented in CPM; extraction uses `tar`), globbing, terminal detection. |

### 3.1 External processes

CPM has no third-party library dependencies. It drives these programs as subprocesses:

| Program | Used for | Discovery |
|---|---|---|
| C++ compiler (`g++`, `clang++`, `cl`) | probing, compilation (through Ninja) | see [build.md §1](build.md#1-toolchain-discovery) |
| `ninja` ≥ 1.10 | executing build plans | `CPM_NINJA`, `PATH`, `$CPM_HOME/tools/ninja` |
| `curl` | `https://` downloads | `PATH` (shipped with Windows 10+, macOS, most Linux distributions) |
| `tar` | extracting `.tar.gz` archives | `PATH` (bsdtar on Windows 10+/macOS, GNU tar on Linux) |
| `git` | git dependencies | `PATH` |

`file://` URLs are handled natively without `curl`. A missing tool is reported only when an operation actually needs it, with a hint naming the tool.

## 4. Core data structures

Sketches below are normative for field meaning, not for exact C++ spelling.

```cpp
namespace cpm {

namespace fs = std::filesystem;

// ---- json ---------------------------------------------------------------
namespace json {
struct Span { std::uint32_t begin = 0, end = 0; };        // byte offsets into the source text
class Value {                                              // null | bool | int64 | double | string | array | object
    // objects keep insertion order: std::vector<std::pair<std::string, Value>>
    // every node remembers its Span (and the key's Span for members) for diagnostics and edits
};
struct ParseOptions { bool comments = true, trailing_commas = true; };
Value parse(std::string_view text, const ParseOptions& = {});   // throws ParseError{line, col, message}
struct WriteOptions { int indent = 2; bool sort_keys = false; };
std::string write(const Value&, const WriteOptions& = {});
namespace edit {                                           // return the edited text and leave everything else byte-for-byte
std::string set_member(std::string_view text, std::span<const std::string> path, const Value& value);
std::string remove_member(std::string_view text, std::span<const std::string> path);
}
}

// ---- semver -------------------------------------------------------------
namespace semver {
struct Version { std::uint64_t major, minor, patch; std::vector<std::string> pre; std::string build; };
class Range {                      // union of disjoint, sorted intervals plus npm prerelease opt-in tuples
public:
    static Range parse(std::string_view);                  // ^ ~ = < <= > >= x * || hyphen ranges
    bool contains(const Version&) const;
    Range intersect(const Range&) const;
    bool empty() const;
};
}

// ---- manifest -----------------------------------------------------------
enum class TargetKind { Executable, StaticLib, SharedLib, HeaderOnly };
enum class Os { Windows, Linux, MacOS };

struct Profile {
    std::string name;
    std::optional<std::string> inherits;
    std::string opt = "0";                                 // "0" "1" "2" "3" "s" "z"
    bool debug = true, assertions = true, lto = false, werror = false, debug_runtime = false;
    std::vector<std::string> sanitizers, defines, cxxflags, ldflags;
    std::string runtime = "dynamic";                       // dynamic | static
    std::string warnings = "default";                      // none | default | strict
};

struct DependencySpec {
    enum class Kind { Registry, Git, Path };
    std::string name;
    Kind kind;
    std::string specifier;                                 // verbatim from the manifest; recorded in the lock
    semver::Range range;                                   // Registry
    std::string registry;                                  // Registry: registry name ("default" if empty)
    std::string git_url, git_ref;                          // Git
    fs::path path;                                         // Path, relative to the declaring manifest
    std::optional<json::Value> overlay;                    // partial manifest for packages without cpm.jsonc
    bool dev = false;
    json::Span span;                                       // where it was declared, for diagnostics
};

struct PlatformOverrides { std::vector<std::string> defines, public_defines, system_libs, sources, cxxflags, ldflags; };

struct Manifest {
    fs::path file, root;                                   // .../cpm.jsonc and its directory
    std::string name;
    semver::Version version;
    std::string std = "c++20";
    std::optional<TargetKind> type;                        // nullopt = infer (manifest.md §4)
    std::optional<fs::path> main;
    std::vector<std::string> sources;                      // globs; empty = defaults
    fs::path include_dir = "include";
    std::vector<std::string> defines, public_defines, system_libs, files;
    bool is_private = false;
    std::map<std::string, std::string> scripts;
    std::vector<DependencySpec> deps, dev_deps;
    std::map<std::string, Profile> profiles;               // built-ins merged with the manifest's
    std::map<std::string, PlatformOverrides> platforms;    // windows | linux | macos | unix
    json::Value raw; std::string text;                     // kept for diagnostics and surgical edits
};

// ---- lock ---------------------------------------------------------------
struct RegistryResolution { std::string registry, url, integrity; };   // integrity = "sha256:<hex>"
struct GitResolution { std::string url, ref, commit; };
struct PathResolution { std::string path; };                            // relative, '/' separated
using Resolution = std::variant<RegistryResolution, GitResolution, PathResolution>;

struct LockedPackage {
    std::string name;
    semver::Version version;
    Resolution resolution;
    TargetKind type;
    std::string std;
    std::map<std::string, semver::Version> dependencies;
    bool dev = false;
};
struct LockedRootDep { std::string specifier; semver::Version version; };
struct Lockfile {
    int lockfile_version = 1;
    std::map<std::string, LockedRootDep> dependencies, dev_dependencies;   // importers["."]
    std::map<std::string, LockedPackage> packages;                         // key "name@version"
};

// ---- toolchain ----------------------------------------------------------
struct Toolchain {
    enum class Family { Gcc, Clang, AppleClang, Msvc, ClangCl } family;
    fs::path cxx, ar;
    std::string version;                                   // "13.2.0"
    std::string target;                                    // "x86_64-w64-mingw32"
    Os os; std::string arch, stdlib, stdlib_abi, crt, eh, threads;
    std::map<std::string, std::string> env;                // extra environment (MSVC vcvars, post-MVP)
};

struct NinjaRule { std::string name, command, description, depfile, deps, rspfile, rspfile_content; };

class Driver {                                             // one per compiler family; see build.md §3
public:
    virtual ~Driver() = default;
    virtual std::vector<NinjaRule> rules() const = 0;      // cxx, ar, link, link_shared
    virtual std::vector<std::string> compile_flags(const Profile&, std::string_view std, bool pic) const = 0;
    virtual std::vector<std::string> link_flags(const Profile&, TargetKind) const = 0;
    virtual std::string define_flag(std::string_view) const = 0;
    virtual std::string include_flag(const fs::path&) const = 0;
    virtual std::vector<std::string> lib_flags(const fs::path& dir, std::string_view name) const = 0;
    virtual std::string object_ext() const = 0;            // ".o" | ".obj"
    virtual std::string exe_file(std::string_view) const = 0;
    virtual std::string static_lib_file(std::string_view) const = 0;
    virtual std::string shared_lib_file(std::string_view) const = 0;
};

struct AbiFingerprint {
    std::vector<std::pair<std::string, std::string>> inputs;   // sorted by key
    std::string hash;                                          // 64 hex
    std::string dir() const { return hash.substr(0, 16); }
};

// ---- build --------------------------------------------------------------
struct ExternalLib {                                       // a dependency as seen by the build
    std::string name;
    fs::path include_dir;                                  // cpm_modules/<name>/include
    std::optional<fs::path> lib_dir;                       // cpm_modules/<name>/lib (absent for header-only)
    std::optional<std::string> link_name;
    std::vector<std::string> public_defines, system_libs;
    bool dev = false;
};

struct BuildTarget {
    std::string name;                                      // "hello", "json.test"
    enum class Kind { Objects, Executable, StaticLib, SharedLib, Test } kind;
    std::vector<fs::path> sources;
    std::vector<std::string> uses;                         // object sets / libraries of other targets linked in
    fs::path output;
};

struct BuildPlan {
    fs::path root;
    std::string build_dir;                                 // cpm_modules/.cache/<profile>: intermediates
    std::string out_dir;                                   // build/<profile>: outputs
    Profile profile;
    Toolchain toolchain;
    AbiFingerprint abi;
    std::vector<fs::path> include_dirs;                    // ordered, see build.md §4.3
    std::vector<std::string> defines;
    std::vector<ExternalLib> libs;                         // topologically ordered, dependencies last
    std::vector<BuildTarget> targets;
};
}
```

## 5. Errors and diagnostics

- Fatal errors are thrown as `cpm::Error { std::string message; std::string hint; std::optional<SourceLoc> loc; }`.
  `main` catches the error, prints it, and exits with code 1 (2 for usage errors). Expected misses such as cache lookups and optional probes use `std::optional`/`std::expected` and never throw.
- Diagnostic format (normative for errors that have a location):

  ```
  error: cpm.jsonc:12:15: unknown target type "exe"
     12 |   "type": "exe",
        |           ^^^^^
    hint: expected one of: executable, static-lib, shared-lib, header-only
  ```
- Warnings use the same shape with `warning:`. `--quiet` suppresses warnings but never errors.
- Subprocess failures report the full command line (with `-v`) and the last 40 lines of its output.

## 6. Implementation constraints

- **Language:** C++23 limited to the intersection supported by GCC 13, Clang 17 and MSVC 19.4x. Allowed: `std::expected` (with monadic operations), `std::format`, `std::span`, `std::filesystem`, concepts, ranges algorithms and views, `std::string::contains`, `std::to_underlying`.
  **Not allowed** (missing in GCC 13): `std::print`/`std::println` (use `std::format` with `std::fwrite`), `std::ranges::to`, explicit object parameters ("deducing this"), `<flat_map>`, `<generator>`, `import std`.
- **No third-party code** in the CPM binary. Test-only dependencies (devDependencies) are allowed once `cpm install` works.
- **Portability:** everything OS-specific lives in `core/` (`proc`, `fsx`, `term`, `paths`) behind portable interfaces, implemented with `#ifdef _WIN32` translation units. Windows code uses wide-character APIs (`…W`) and converts at the boundary. UTF-8 is used everywhere else.
- **Style:** `#pragma once`, `.hpp`/`.cpp`, `snake_case` for functions and variables, `PascalCase` for types, one module per directory, no `using namespace` in headers.

## 7. Bootstrap and self-hosting

The CPM repository is itself a CPM project (`cpm.jsonc`, `src/main.cpp`, `tests/*.test.cpp`) from its first commit.

| Stage | How it is built | Output |
|---|---|---|
| 0 | `scripts/bootstrap.ps1` / `scripts/bootstrap.sh`: one compiler invocation, `g++ -std=c++23 -O1 -Isrc <all src/**/*.cpp> -o cpm_modules/.cache/bootstrap/cpm[.exe]`. The compiler is taken from `CXX` or defaults to `g++`/`clang++`. | `cpm_modules/.cache/bootstrap/cpm` |
| 1 | `stage0 build --release` | `build/release/cpm` |
| 2 | `stage1 build --release --clean` (stage 1 is first copied to `cpm_modules/.cache/stage1/`) | same path; `stage2 --version` and `stage2 test` MUST pass |

After stage 1 exists, CPM is developed with itself (`cpm build`, `cpm test`, `cpm run`). The bootstrap script is kept working for fresh clones and CI.
CPM's only dependency is its test framework, the devDependency `doctest` (git, pinned in `cpm-lock.json`). Stage 1 installs it into the store like any other package, so the first build on a machine with an empty store needs network access once. The bootstrap itself never does.
When CPM rebuilds the executable that is currently running (Windows forbids overwriting it), it first renames that file to `<name>.old` and deletes stale `.old` files on the next run (see [build.md §6](build.md#6-running-ninja)).

## 8. Repository layout

```
cpm.jsonc  cpm-lock.json  README.md  LICENSE  .gitignore  .gitattributes
docs/spec/       this specification (+ examples/)
schema/          JSON Schemas (manifest, lockfile, registry index)
src/             main.cpp and the modules of §3
tests/           *.test.cpp, support/ (check.hpp over doctest; projects and registries built at test time)
scripts/         bootstrap.{ps1,sh} (stage 0), selfhost.{ps1,sh}
.github/         workflows/ci.yml (self-hosting check per toolchain), workflows/release.yml (tagged releases)
```

## 9. Milestones

| # | Scope | Exit criterion |
|---|---|---|
| M0 | Skeleton: CPM's own `cpm.jsonc`, `main.cpp` (`--version`, `--help`), `cli/args`, `core/{error,log,term}`, bootstrap scripts, `.gitignore`, `tests/support/check.hpp` | `bootstrap` produces a working `cpm --version` |
| M1 | `cpm build` and `cpm test` for dependency-free projects: `json` parse, `manifest`, `core/{proc,fsx,paths,glob,sha256}`, `toolchain/{probe,gcc_driver}`, `build/{layout,plan,ninja_writer,ninja_gen,runner,pipeline,tests}` | **Self-hosting:** stage 0 → 1 → 2, and stage 2 passes `cpm test` (`scripts/selfhost`) |
| M2 | `cpm init`, `cpm run`: modules `init/` (templates, questions) and `run/` (script and program commands) | `cpm init -y && cpm run` works in an empty directory, and every template builds and passes its own test |
| M3 | `cpm install` I and `cpm remove`: `semver` ranges, `json` edit, `lock`, `install/{sources,git,resolver,store,dep_builder,linker,installer}`, `toolchain/abi`, `core/filelock`, directory links | path and git dependencies (header-only and static-lib): git packages are built once per ABI in the store and shared between projects; path packages compile in the project's graph |
| M4 | `cpm install` II: static registry (`file://`, `https://`), archive integrity, registry overlays, prebuilt binaries, `--prefer-offline`, `--no-build` | end-to-end install tests green (`install.test`, `registry.test`: real installs and builds against `file://` registries); CPM's tests use `doctest` installed by CPM |
| M5 | Registry tooling: `cpm publish` (`publish/`, `core/archive`), `cpm update`, a built-in default registry URL, and the `cpm-registry` repository seeded with fmt, spdlog, nlohmann-json, doctest, Catch2 and CLI11 | every seeded package publishes reproducibly, installs from a fresh home and runs its smoke program (`cpm-registry/scripts/verify.ps1`) |
| M6 | Registry service: the API of registry.md §7, `cpm login`/`logout`/`whoami` (GitHub device flow, `$CPM_HOME/credentials.json`), `cpm publish` to services, `cpm yank`, `cpm owner`, and the `cpm-registry` Cloudflare Worker (D1, R2, GitHub sign-in, website) | the seed packages publish through the API to a local `wrangler dev`, then install and run from an empty CPM home; the service's test suite passes |
| M7+ | *post-MVP:* MSVC driver, PubGrub, `cpm publish --with-binary`, `cpm outdated`, `cpm search`, symbol isolation, Legacy Bridge, parallel fetch and dependency builds, Ninja auto-download, workspaces | — |
