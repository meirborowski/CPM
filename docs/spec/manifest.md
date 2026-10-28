# Manifest: `cpm.jsonc`

Every CPM package has a `cpm.jsonc` at its root. The smallest valid manifest is:

```jsonc
{ "name": "hello", "version": "0.1.0" }
```

Everything else is inferred from the directory layout (§4) or has a default.

## 1. File format

- The file is named exactly `cpm.jsonc` and is encoded as UTF-8. A leading BOM is accepted and ignored.
- The format is JSON (RFC 8259) plus three extensions:
  - `// line comments`
  - `/* block comments */` (they do not nest)
  - trailing commas in arrays and objects
- **Duplicate keys are an error** at any depth.
- Numbers must fit in a 64-bit integer or an IEEE double. No manifest field needs a non-integer number.
- Keys starting with `$` are reserved. `$schema` is accepted and ignored by CPM, so editors can use it for completion. All other `$` keys produce a warning.
- **Unknown keys produce a warning**, not an error, so newer manifests still load in older CPM versions. The JSON Schema rejects them so that editors flag typos.

## 2. Example

The same manifest, together with its lockfile and registry index, is in [examples/](examples/hello/cpm.jsonc).

```jsonc
{
  "$schema": "./schema/cpm.schema.json",   // optional, for editor completion
  "name": "hello",
  "version": "0.1.0",
  "description": "Says hello",
  "license": "MIT",
  "std": "c++23",
  "type": "executable",                    // inferred from src/main.cpp if omitted

  "scripts": {
    "start": "cpm run",
    "fmt": "clang-format -i src/*.cpp include/**/*.hpp"
  },

  "dependencies": {
    "fmt": "^10.2.0",                      // default registry
    "json": {                              // git package without a cpm.jsonc: overlay supplies the manifest
      "git": "https://github.com/nlohmann/json.git",
      "tag": "v3.11.3",
      "manifest": { "type": "header-only", "std": "c++11", "include": "single_include" }
    },
    "mylib": "file:../mylib"               // local path
  },
  "devDependencies": {
    "doctest": "^2.4.11"
  },

  "profiles": {
    "debug":   { "sanitizers": ["address", "undefined"] },
    "release": { "lto": true },
    "profiling": { "inherits": "release", "debug": true }
  },

  "platforms": {
    "windows": { "systemLibs": ["ws2_32"] },
    "unix":    { "systemLibs": ["pthread"] }
  }
}
```

## 3. Field reference

| Field | Type | Default | Description |
|---|---|---|---|
| `name` | string | **required** | Package name, see §3.1. |
| `version` | string | **required** | SemVer 2.0.0 version without a leading `v` ([dependencies.md §2](dependencies.md#2-versions)). |
| `description` | string | `""` | One-line summary. |
| `license` | string | — | SPDX license expression. |
| `authors` | string[] | `[]` | `"Name <email>"` entries. |
| `repository`, `homepage` | string | — | URLs. |
| `keywords` | string[] | `[]` | Search keywords for registries. |
| `private` | bool | `false` | `true` makes `cpm publish` refuse the package. |
| `std` | string | `"c++20"` | C++ standard: `c++11`, `c++14`, `c++17`, `c++20`, `c++23`, `c++26`. For the root package it is the standard **all C++ code** is compiled with. For a dependency it is the **minimum** standard that package needs, and a dependency that sets `cstd` but not `std` needs none (§3.2). |
| `cstd` | string | `"gnu17"` | C standard for the package's `.c` files: `c89`, `c99`, `c11`, `c17`, `c23`, or the GNU dialects `gnu89`, `gnu99`, `gnu11`, `gnu17`, `gnu23`. Every package's C code is compiled with its own `cstd` (§3.3). |
| `type` | string | inferred | `executable`, `static-lib`, `shared-lib`, `header-only` (§4). |
| `toolchain` | string or string[] | — | The compiler the project asks for when neither the command line nor `CXX` choose one: a request such as `"gcc >=14"`, or several in order of preference, `["gcc >=14", "clang >=18"]` ([build.md §1.1](build.md#11-selection-order)). Only the root package's is used. |
| `main` | string | inferred | Entrypoint source, relative to the package root (§4). |
| `sources` | string[] | `["src/**/*.cpp", "src/**/*.cc", "src/**/*.cxx", "src/**/*.c"]` | Glob patterns of translation units (§5). `.c` files are C, the others C++. |
| `include` | string | `"include"` | Public header directory, exported to dependents. It need not exist for executables. |
| `defines` | string[] | `[]` | Private preprocessor definitions, `NAME` or `NAME=VALUE`. |
| `publicDefines` | string[] | `[]` | Definitions applied to this package **and** everything that depends on it. |
| `systemLibs` | string[] | `[]` | System libraries to link, as bare names (`pthread`, `ws2_32`, `dl`). They are propagated to dependents of static libraries. |
| `files` | string[] | see [registry.md §6](registry.md#6-publishing) | Globs of files included when publishing (`cpm.jsonc` and license and readme files are always included). |
| `scripts` | object | `{}` | Map of script name to shell command, run by `cpm run <name>` ([cli.md](cli.md#cpm-run)). |
| `dependencies` | object | `{}` | Map of package name to specifier ([dependencies.md §1](dependencies.md#1-dependency-specifiers)). |
| `devDependencies` | object | `{}` | Same as `dependencies`, but used only for tests and only when this package is the root. |
| `overrides` | object | `{}` | Version ranges that replace dependencies' requirements: `"fmt": "^10.2.0"`, or `"spdlog>fmt": "^10.2.0"` for one dependent ([dependencies.md §4.6](dependencies.md#46-overrides)). Only the project being built uses them, and `cpm publish` refuses a package that has them. |
| `profiles` | object | built-ins | Build profiles (§6). |
| `platforms` | object | `{}` | Per-OS additions (§7). |

### 3.1 Package names

- The name must match `^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$`: 1–64 characters, lowercase ASCII letters, digits, `.`, `_`, `-`, starting and ending with a letter or digit.
- These names are rejected because they collide with Windows device names: `con`, `prn`, `aux`, `nul`, `com1`–`com9`, `lpt1`–`lpt9`.
- The name doubles as the target name. It is used for the executable file, the library file (`lib<name>.a`) and the `cpm_modules/<name>` directory.
- Scoped names (`@scope/name`) are reserved for a future version.

### 3.2 `std` semantics

- The root package's `std` is passed to the compiler for the C++ code of the root and of **every dependency built for it**. It is also part of the ABI fingerprint ([abi.md](abi.md)).
- A dependency whose `std` is newer than the root's is an error, for example `fmt requires c++23 but the project uses c++20`.
- A dependency whose `std` is older is compiled with the root's newer standard.
- A dependency that sets `cstd` and not `std` is a C library, which needs no particular C++ standard. Its `std` is `c++11`, the oldest, so it never restricts the project. That is the `std` that `cpm publish` declares and the lockfile records. A package whose C++ code needs a newer standard sets `std` as well. The root's `std` always defaults to `c++20`, since it is what its C++ dependencies are compiled with.

### 3.3 `cstd` semantics

- A package's `.c` files are compiled with **its own** `cstd`, whether it is the root, a path dependency or a package from the store. The root's `cstd` doesn't apply to dependencies.
- That differs from `std` because C code often needs the standard it was written for: C23, for example, removed K&R-style function definitions and made `bool`, `true` and `false` keywords. C's ABI doesn't depend on the standard, so C code compiled with different standards links together, and `cstd` isn't part of the ABI fingerprint ([abi.md](abi.md)).
- The GNU dialects add the compiler's extensions. With glibc they also declare POSIX functions such as `strdup` and `fileno` without feature-test macros, which much existing C code expects.
- The default, `gnu17`, is what GCC 8 to 14 and recent Clang releases use when given no `-std`, so C code that builds with a plain `gcc -c` builds with CPM. It is fixed, so that builds don't change when a compiler changes its own default, as GCC 15 did to `gnu23`.

## 4. Target inference

If `type` is absent, CPM infers it in this order. The first match wins:

1. `main` is set → its file name decides: `main.*` ⇒ `executable`, `lib.*` ⇒ `static-lib`. Any other name requires an explicit `type`.
2. `src/main.cpp` (or `.cc`, `.cxx`, `.c`, tried in that order) exists ⇒ `executable`, `main` = that file.
3. `src/lib.cpp` (or `.cc`, `.cxx`, `.c`, tried in that order) exists ⇒ `static-lib`, `main` = that file.
4. No source matches the `sources` globs and the `include` directory exists ⇒ `header-only`.
5. Otherwise: error `cannot infer target type`, with a hint to add `src/main.cpp`, `src/lib.cpp` or `"type"`.

If both `src/main.cpp` and `src/lib.cpp` exist and `type` is absent, CPM picks `executable` and warns.

**Targets produced per type:**

| Type | Main target | Test executables link against |
|---|---|---|
| `executable` | `<name>[.exe]` = entrypoint + **core objects** (every source except the entrypoint) | core objects |
| `static-lib` | `lib<name>.a` / `<name>.lib` from all sources | the library |
| `shared-lib` | `lib<name>.so` / `.dylib` / `.dll`, or MSVC's `<name>.dll` (+ import library) from all sources. What it exports is marked with its `<NAME>_API` (§4.1) | the library |
| `header-only` | none (headers only) | nothing extra |

Executables split their sources so that tests can exercise internal code: everything except the entrypoint compiles once and links into both the executable and every test.

**Tests:** each `tests/*.test.cpp` (non-recursive; `.cc`, `.cxx` and `.c` also accepted) becomes its own executable named after the file stem, for example `tests/json.test.cpp` → `json.test[.exe]`. Files in subdirectories of `tests/` are helpers and are never test entrypoints. Tests are built only by `cpm test` or `cpm build --tests` ([build.md §7](build.md#7-tests)).

### 4.1 Exported symbols: `<NAME>_API`

A shared library has to say which functions, classes and variables it exports: MSVC exports nothing else. CPM gives every library package a macro for that, so a library is written the same way for every compiler and whether it is built static or shared:

```cpp
// include/cpm-json/value.hpp in the package "cpm-json"
class CPM_JSON_API Value { … };
CPM_JSON_API Value parse(std::string_view text);
```

- **The name** is the package name in upper case, with every character that isn't a letter or a digit replaced by `_`, followed by `_API`: `cpm-json` gives `CPM_JSON_API`, `fmt` gives `FMT_API`.
- **Where it is defined:** in every compile that can see the package's headers, which are the package's own sources and tests, and everything that depends on it. CPM passes it as a definition ([build.md §4.3](build.md#43-include-directories-definitions-and-link-order)).
- **Its value:**

| The package is | Its own sources | Its tests | Everything else |
|---|---|---|---|
| a `shared-lib` built as the root package | `__declspec(dllexport)` on Windows, `__attribute__((visibility("default")))` elsewhere | `__declspec(dllimport)` on Windows, `__attribute__((visibility("default")))` elsewhere | — |
| a `static-lib`, a `header-only` library, or any library built from a path dependency (which is always compiled into an archive, [build.md §4.4](build.md#44-path-dependencies)) | empty | empty | empty |

- **On MinGW,** a shared library that marks nothing still exports everything, as before. Once it marks something, only what it marks is exported, as with MSVC.
- **With MSVC,** a shared library that marks nothing exports nothing, so `link.exe` writes no import library. The build then stops with an error that says to mark the library's API with its macro.
- A package that defines a macro of that name itself, in `defines` or in a header, gets an error from the compiler for the redefinition. The package must rename its own macro.

## 5. Sources and include directories

- **Globs** are relative to the package root, use `/`, and support `*` (no `/`), `**` (any depth, including zero), `?`, and `{a,b}`. A leading `!` excludes. Patterns are applied in order.
- Matching is case-sensitive on every OS. Results are sorted byte-wise so build plans are deterministic.
- Directories named `cpm_modules` or `.git` are never searched, and links (symlinks, junctions) are never followed.
- Entrypoint files (`main`, test files) are always removed from the core set even if a glob matches them.
- **Languages:** a source whose name ends in `.c` is C, compiled with the C compiler ([build.md §1.2](build.md#12-family-and-companion-tools)) and the package's `cstd`. Every other source is C++. Matching is case-sensitive here too, so `.C` is C++, as GCC treats it. A package can mix both; C functions that C++ code calls need `extern "C"` declarations, as usual.
- **Include path order for the package's own translation units:**
  1. `src/` (private headers, always)
  2. the package's `include` directory, if it exists
  3. dependency include directories, in dependency order ([build.md §4.3](build.md#43-include-directories-definitions-and-link-order))
  4. for test translation units only: `tests/`
- Only the `include` directory is exported to dependents. `src/` never is.

## 6. Profiles

A profile is a named set of abstract build settings. Two profiles always exist:

| Field | Type | `debug` (built-in) | `release` (built-in) | Meaning |
|---|---|---|---|---|
| `inherits` | string | — | — | Parent profile. **Required** for custom profiles. |
| `opt` | `0`–`3`, `"0"`–`"3"`, `"s"`, `"z"` | `0` | `3` | Optimisation level. |
| `debug` | bool | `true` | `false` | Emit debug information. |
| `assertions` | bool | `true` | `false` | `false` defines `NDEBUG`. |
| `lto` | bool | `false` | `false` | Link-time optimisation (root targets only; dependencies are never built with LTO). |
| `sanitizers` | string[] | `[]` | `[]` | Any of `address`, `undefined`, `thread`, `memory`, `leak`. Unsupported combinations warn and are dropped. |
| `runtime` | `"dynamic"`/`"static"` | `"dynamic"` | `"dynamic"` | C/C++ runtime linkage. |
| `debugRuntime` | bool | `false` | `false` | MSVC only: use the debug CRT (`/MDd`). Changes the ABI fingerprint. |
| `warnings` | `"none"`/`"default"`/`"strict"` | `"default"` | `"default"` | Warning level ([build.md §3](build.md#3-profile-to-flag-mapping)). |
| `werror` | bool | `false` | `false` | Treat warnings as errors. |
| `defines` | string[] | `[]` | `[]` | Extra definitions for this profile. |
| `cflags`, `cxxflags`, `ldflags` | string[] | `[]` | `[]` | Raw flags for C compiles, C++ compiles and links. These are escape hatches and are not portable. |

**Merging rules:**

1. A manifest entry named `debug` or `release` is merged over the built-in profile of that name.
2. A custom profile is merged over its `inherits` parent after the parent itself has been resolved. Cycles and unknown parents are errors.
3. Scalars override. Arrays **append**, removing duplicates and keeping the first occurrence.
4. Custom profile names match `^[a-z][a-z0-9-]{0,31}$`.
5. **Only the root package's profiles are used.** `profiles` in dependency manifests are ignored.

## 7. Platform overrides

`platforms` keys: `windows`, `linux`, `macos`, and `unix` (= linux and macos). Each value may contain
`defines`, `publicDefines`, `systemLibs`, `sources`, `cflags`, `cxxflags` and `ldflags`. These are **appended** to the base fields, in this order: base, `unix` (if it applies), the specific OS.
Platform-specific dependencies are reserved for a future version.

## 8. Manifest overlays

An *overlay* is a partial manifest supplied from outside a package that has no `cpm.jsonc`, or whose `cpm.jsonc` is wrong. It can come from a registry index entry ([registry.md §3](registry.md#3-index-documents)) or from the object form of a git or path dependency ([dependencies.md §1.2](dependencies.md#12-object-form)).

- Allowed overlay fields: `version`, `type`, `std`, `cstd`, `main`, `sources`, `include`, `defines`, `publicDefines`, `systemLibs`, `platforms`, `dependencies`, and `build` for the Legacy Bridge (*post-MVP*, [build.md §8](build.md#8-legacy-bridge-post-mvp)).
- `name` always comes from the dependency key. `version` comes from the registry entry, or the overlay, or the package's own manifest, or is derived from the git ref ([dependencies.md §1.3](dependencies.md#13-versions-of-git-and-path-packages)).
- If the package also has a `cpm.jsonc`, overlay fields replace the corresponding top-level fields. They are not merged deeply.

## 9. Validation

CPM validates the manifest after parsing and reports **all** problems it finds, not only the first, in the diagnostic format of [architecture.md §5](architecture.md#5-errors-and-diagnostics). Errors include:

- missing or invalid `name` or `version`; an unknown `type`, `std`, `cstd` or profile value
- a dependency that appears in both `dependencies` and `devDependencies`
- a dependency on the package itself
- a `toolchain` request that isn't a family (`gcc`, `clang`, `apple-clang`, `msvc`) optionally followed by a version range
- an `overrides` key that isn't a package name or `<dependent>><name>` with two different names, or a value that isn't a version range
- a `main` that does not exist, or an explicit `type` that contradicts the files on disk (`executable` without an entrypoint)
- `profiles` problems: a custom profile without `inherits`, an inheritance cycle, an invalid name

## 10. Edits made by CPM

`cpm install <spec>` and `cpm remove <name>` change `cpm.jsonc` through **surgical text edits** (`json::edit`). Every byte outside the edited member stays as it was, comments included.

- **Updating** a member replaces only its value span.
- **Inserting** into `dependencies`/`devDependencies`: if the existing keys are sorted, the new key goes at its sorted position; otherwise it is appended. Indentation and the newline style are copied from the sibling entries (default: 2 spaces, LF). Separating commas are added or adjusted as needed. A trailing-comma style, if present, is kept.
- If the object is missing, it is created as the last member of the root object.
- **Removing** a member also removes its separating comma and any comments on the same line after it.
- Manifest edits are written atomically and only after the install pipeline has succeeded ([cli.md](cli.md#cpm-install)).
