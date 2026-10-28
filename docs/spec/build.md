# Build: toolchains, profiles and Ninja generation

## Overview: the `cpm build` pipeline

1. **Locate** the project root (walk up from the working directory to the nearest `cpm.jsonc`) and load the manifest.
2. **Resolve the profile** (default `debug`): built-ins ← manifest ← CLI flags ([manifest.md §6](manifest.md#6-profiles)).
3. **Select and probe the toolchain** (§1), and its C compiler when anything in the build has `.c` sources (§1.2). Probes are cached, so a warm run spawns no processes.
4. **Compute the ABI fingerprint** ([abi.md](abi.md)).
5. **Ensure dependencies:** if the lockfile is stale, or `.cpm-modules.json` does not match the lock hash and ABI, run the install pipeline (offline when everything is cached). It builds any missing `lib/<abi16>` (§5) and repoints the `lib` links ([store.md §5](store.md#5-cpm_modules)).
6. **Plan:** infer targets from the layout, expand the source globs, and compute include directories, definitions and link order (§4).
7. **Generate** `cpm_modules/.cache/<profile>/build.ninja` (§4.5). Write it only if its content changed, and refresh `compile_commands.json`.
8. **Run Ninja** (§6) and report.

Steps 1–7 make up the no-op cost. They MUST stay free of subprocesses on a warm cache, apart from Ninja itself.

## 1. Toolchain discovery

### 1.1 Selection order

The first of these that makes a choice wins:

1. **The command:** `--toolchain <choice>`, or `--cxx <compiler>` ([cli.md](cli.md#cpm-toolchain)).
2. **The environment:** `CXX`, a compiler name or path.
3. **The project:** `"toolchain"` in its `cpm.jsonc` ([manifest.md §3](manifest.md#3-field-reference)).
4. **The machine:** `"cxx"` or `"toolchain"` in `$CPM_HOME/config.jsonc`, which `cpm toolchain use` and `cpm setup` write ([cli.md §5](cli.md#5-configuration)). When both are there, `"cxx"` wins.
5. **Automatic:** `clang++`, then `g++`, on `PATH`; then, on Windows, the newest MSVC that Visual Studio has installed (§1.4). A `clang++` that targets the MSVC ABI (`*-windows-msvc`, as LLVM's Windows installer and Visual Studio install it) is skipped: CPM supports the MSVC ABI through `cl.exe`, not through Clang.

A *choice* is one of two things:
- **A compiler:** a program name, looked up on `PATH` (`g++-14`, `clang++`), or a path. On Windows the lookup tries only `.exe` and `.com`, the extensions `CreateProcess` can start directly, rather than every `PATHEXT` entry.
- **A request:** a family, optionally followed by a version range: `gcc`, `clang >=18`, `apple-clang`, `gcc 14`, `msvc >=19.44` (§1.4). The families are `gcc`, `clang`, `apple-clang` and `msvc`. `clang` means LLVM's Clang, not Apple's. `msvc` means Visual Studio's `cl.exe`. Its version is the compiler's (`19.51.36260`), not Visual Studio's (`18.11`) or the toolset's (`14.51`). A project's `"toolchain"` takes only requests, since paths differ between machines. It can also list several requests, which are tried in order: `["gcc >=14", "clang >=18", "apple-clang >=16"]`.

The resolved absolute path identifies the toolchain from then on.

- **What chose it.** Every build says which compiler it uses and what chose it: `Compiling hello v0.1.0 (debug, gcc 14.2.0, from cpm.jsonc)`. The sources read `from --toolchain`, `from --cxx`, `from CXX`, `from cpm.jsonc`, `from config.jsonc`, `found on PATH` and `found in Visual Studio`. With `-v` the line also gives the compiler's path.
- **Disagreements.** When the command or `CXX` chooses a compiler that the project's `"toolchain"` doesn't accept, CPM warns and uses the chosen compiler, so a project can still be tried with other compilers.
- **Nothing matches.** A request that no compiler on the machine meets is an error. It names the request and what made it, lists the compilers CPM found, and says how to get one: `cpm setup --compiler gcc` or `cpm setup --compiler clang` where `cpm setup` can install it ([cli.md](cli.md#cpm-setup)).
- A Clang that targets the MSVC ABI (`clang-cl`, or a `clang++` for `*-windows-msvc`), named explicitly, is an error with a hint to use `--toolchain msvc` or a MinGW toolchain.

### 1.2 Family and companion tools

- The family comes from the probe macros: `__clang__` with `__apple_build_version__` ⇒ `apple-clang`, `__clang__` ⇒ `clang`, `__GNUC__` ⇒ `gcc`, `_MSC_VER` with no `__clang__` ⇒ `msvc`, `clang-cl` ⇒ `clang-cl`.
- **Archiver:** for GCC, `gcc-ar` from the compiler's directory with the same prefix and suffix as the compiler (`x86_64-w64-mingw32-g++-13` → `x86_64-w64-mingw32-gcc-ar-13`), falling back to `ar`. For Clang, `llvm-ar` (same directory, then `PATH`), falling back to `ar`. For MSVC, `lib.exe`. Using `gcc-ar`/`llvm-ar` keeps LTO archives working.
- **C compiler:** the C++ compiler's companion in the same directory. Its file name is the C++ compiler's with `clang++` replaced by `clang`, `g++` by `gcc`, or `c++` by `cc`, keeping any prefix and suffix: `x86_64-w64-mingw32-g++-13` → `x86_64-w64-mingw32-gcc-13`, `clang++-18` → `clang-18`, `c++` → `cc`. For MSVC it is `cl.exe` itself.
  - CPM looks for it only when the build compiles C: the root, a path dependency, or a package being built in the sandbox (§5) has `.c` sources.
  - It MUST report the same family, version and target as the C++ compiler, so that C and C++ objects come from one toolchain. When it is missing, or reports something else, the build fails with an error that names both compilers and what each one reported.
  - `CC` is not read. The C++ compiler choice (§1.1) picks the toolchain, and the C compiler comes with it.
- **MSVC's tools:** `lib.exe` (the archiver) and `link.exe` (the linker) come from the directory of the `cl.exe` chosen, by absolute path, so that another `link` on `PATH` (Git for Windows has one) is never taken.
- **Linker:** the C++ compiler driver (`link.exe` for MSVC), also for targets whose own code is all C. It links the C++ runtime that mixed targets and C++ dependencies need, so CPM doesn't have to track which packages contain C++. On platforms where the linker keeps shared libraries that nothing uses, a pure C program then lists the C++ runtime among its dependencies.

### 1.3 Probe cache

`$CPM_HOME/toolchains.json` maps the absolute compiler path to the probe result:

```json
{
  "toolchainsVersion": 1,
  "toolchains": {
    "C:/Strawberry/c/bin/g++.exe": {
      "stamp": { "size": 2482688, "mtime": 1705312000 },
      "family": "gcc",
      "version": "13.2.0",
      "target": "x86_64-w64-mingw32",
      "ar": "C:/Strawberry/c/bin/gcc-ar.exe",
      "stdFlags": { "c++20": "-std=c++20", "c++23": "-std=c++23" },
      "inputs": { "arch": "x86_64", "compiler": "gcc", "crt": "ucrt", "eh": "seh", "os": "windows", "stdlib": "libstdc++", "stdlib.abi": "cxx11", "threads": "posix" },
      "c": {
        "path": "C:/Strawberry/c/bin/gcc.exe",
        "stamp": { "size": 2479104, "mtime": 1705312000 },
        "cstdFlags": { "gnu17": "-std=gnu17", "c23": "-std=c2x" }
      }
    }
  }
}
```

An MSVC entry also records:
- its Visual Studio installation and toolset;
- its environment (§1.5);
- the text that starts `/showIncludes` lines, which localised installations translate.

```json
"C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe": {
  "stamp": { "size": 1081976, "mtime": 1774137600 },
  "family": "msvc",
  "version": "19.51.36260",
  "target": "x86_64-pc-windows-msvc",
  "ar": ".../bin/Hostx64/x64/lib.exe",
  "linker": ".../bin/Hostx64/x64/link.exe",
  "vs": { "path": "C:/Program Files/Microsoft Visual Studio/18/Community", "name": "Visual Studio Community 2026", "version": "18.11.0", "toolset": "14.51.36231" },
  "env": { "INCLUDE": "…", "LIB": "…", "LIBPATH": "…", "PATH": "…" },
  "depsPrefix": "Note: including file:",
  "stdFlags": { "c++20": "/std:c++20", "c++23": "/std:c++23preview" },
  "inputs": { "arch": "x86_64", "compiler": "msvc", "crt": "ucrt", "os": "windows", "stdlib": "msvc-stl", "stdlib.abi": "145" }
}
```

An entry is reused while the compiler file's size and mtime are unchanged. `stdFlags` records the accepted spelling of each standard the first time it is requested. The final name (`-std=c++23`) is tried first, then the provisional one (`-std=c++2b`, `-std=c++2c`). MSVC's spellings are in §3.

`c` is added the first time a build needs the C compiler (§1.2), and probed again when the C compiler's own size or mtime change. `cstdFlags` works like `stdFlags`: `-std=c23` is tried before `-std=c2x`, and `-std=gnu23` before `-std=gnu2x`. The other standards have one spelling.

### 1.4 Finding compilers

`cpm toolchain list` and requests (§1.1) look for compilers in these places, in this order:

1. every directory on `PATH`: `g++` and `clang++`, and versioned names such as `g++-14` and `clang++-18` (`.exe` on Windows);
2. `$CPM_HOME/tools`: the `bin` directories of the tools `cpm setup` installed (`<tool>/bin` and `<tool>/mingw64/bin`);
3. on Windows, MSYS2's environments: `ucrt64/bin`, `clang64/bin` and `mingw64/bin` under `C:/msys64`, or under `MSYS2_ROOT` when it is set;
4. on macOS, Homebrew's LLVM, which isn't on `PATH`: `/opt/homebrew/opt/llvm/bin` and `/usr/local/opt/llvm/bin`;
5. on Windows, Visual Studio and its Build Tools:
   - CPM asks `vswhere.exe` (in `%ProgramFiles(x86)%/Microsoft Visual Studio/Installer/`) for every installation that has the C++ tools: `vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8`.
   - Each MSVC toolset an installation has, `VC/Tools/MSVC/<toolset>/`, gives a `cl.exe` in `bin/Hostx64/x64/` (`bin/Hostarm64/arm64/` on ARM64 Windows).
   - A `cl.exe` on `PATH`, as in a Developer Command Prompt, is found under the name `cl` in step 1.

- A program found under two names, such as `g++` and a `g++-13` it links to, is listed once, under the first name found.
- Each compiler is probed (§1.3, so only new or changed compilers run). A compiler is *usable* when it is GCC, MSVC, or a Clang that doesn't target the MSVC ABI. One older than the versions CPM supports (GCC 13, Clang 17, Apple Clang 15, MSVC 19.40 from Visual Studio 2022 17.10) is still usable, with a note, since many projects need less than C++23.
- **Matching a request:** among the usable compilers of the requested family whose version is in the range, the one with the highest version wins. A tie goes to the one found first. Versions compare as `MAJOR.MINOR.PATCH`, and ranges are written as in [dependencies.md §3](dependencies.md#3-ranges), so `14` means any 14.x and `>=13` any version from 13 on.
- Looking for compilers lists directories and reads the probe cache. It runs no programs on a warm cache, so builds that use requests keep their no-op cost (overview). `vswhere`'s answer is cached in `toolchains.json` too. It is asked again only when the Visual Studio Installer's instance directory (`%ProgramData%/Microsoft/VisualStudio/Packages/_Instances`) has changed, which happens when an installation is added, changed or removed.

### 1.5 MSVC's environment

`cl.exe` and `link.exe` find the C++ library, the Windows SDK and the C runtime through the environment variables `INCLUDE`, `LIB` and `LIBPATH`, and they need some directories on `PATH`. A Developer Command Prompt sets these by running `vcvarsall.bat`. CPM doesn't need one:

- **Capture.** For each MSVC toolset it uses, CPM runs once: `cmd /d /c "call "<vs>/VC/Auxiliary/Build/vcvarsall.bat" <arch> -vcvars_ver=<toolset> >nul 2>nul && set"`, which takes a few seconds. `<arch>` is `x64`, or `arm64` on ARM64 Windows.
- **Cache.** It keeps `INCLUDE`, `LIB`, `LIBPATH`, `EXTERNAL_INCLUDE`, and the directories `vcvarsall.bat` put in front of `PATH`, in the probe cache (§1.3). `EXTERNAL_INCLUDE` names the toolchain's own headers (the standard library and the Windows SDK), which are then *external*: with `/external:W0` (§3) no warning comes from them, as GCC and Clang report none from system headers.
- **Use.** That environment is used for the compiler's probes, Ninja (§6), dependency builds (§5), `cpm test` (§7) and `cpm run`. `INCLUDE`, `LIB`, `LIBPATH` and `EXTERNAL_INCLUDE` replace the inherited values, and the `PATH` directories go in front of the inherited `PATH`. An environment cached before CPM 0.4.9, without `EXTERNAL_INCLUDE`, is captured again.
- **Refresh.** It is captured again when `cl.exe` changes (its stamp), or when one of its `INCLUDE` or `LIB` directories no longer exists, as after a Windows SDK is uninstalled.
- **Developer Command Prompt.** There, CPM still uses its own captured environment for the toolset it chose, so a build never depends on which prompt started it.

## 2. Drivers

A **driver** turns the abstract plan into concrete command lines for one compiler family ([architecture.md §4](architecture.md#4-core-data-structures), `cpm::Driver`):

| Responsibility | GCC / Clang driver | MSVC driver |
|---|---|---|
| Compile rule and header dependencies | `-MD -MF $out.d`, `deps = gcc` | `/showIncludes`, `deps = msvc`, `msvc_deps_prefix` probed (it is localised) |
| Object extension | `.o` | `.obj` |
| Executable | `<name>` / `<name>.exe` | `<name>.exe` |
| Static library | `lib<name>.a` (including MinGW) | `<name>.lib` |
| Shared library | `lib<name>.so`, `lib<name>.dylib`, MinGW: `lib<name>.dll` + import library `lib<name>.dll.a` | `<name>.dll` + import library `<name>.lib`: `link /nologo /INCREMENTAL:NO /DLL /IMPLIB:<name>.lib /OUT:$out @$out.rsp` |
| Library search | `-L<dir> -l<name>` | `/LIBPATH:<dir> <name>.lib` |
| Response files | `@file` | `@file` |
| Archive and link | `gcc-ar`/`llvm-ar` `rcs`; the C++ compiler links | `lib /nologo /OUT:$out @$out.rsp`; `link /nologo /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /OUT:$out @$out.rsp`. `/SUBSYSTEM:CONSOLE` lets `main()` come from a library, as cpm-test's does |
| Environment | inherited | captured from `vcvarsall.bat` (§1.5) |

## 3. Profile to flag mapping

| Setting | GCC / Clang | MSVC |
|---|---|---|
| `std` (C++ only) | `-std=c++11` … `-std=c++26` (spelling from the probe cache) | `/std:c++14` (also used for `c++11`, the oldest MSVC mode), `/std:c++17`, `/std:c++20`. For `c++23`, the first that is accepted of `/std:c++23`, `/std:c++23preview` and `/std:c++latest`. For `c++26`, `/std:c++latest`. The spelling is found as for GCC (§1.3): MSVC answers a flag it doesn't know with warning D9002 |
| `cstd` (C only, the package's own) | `-std=c89` … `-std=gnu23` (spelling from the probe cache) | `/std:c11`, `/std:c17`, `/std:clatest` for `c23`. `c89` and `c99` use MSVC's default C mode, and each GNU dialect maps to its ISO standard |
| `opt` `0` / `1` / `2` / `3` / `s` / `z` | `-O0` / `-O1` / `-O2` / `-O3` / `-Os` / `-Oz` | `/Od` / `/O2` / `/O2` / `/O2` / `/O1` / `/O1` |
| `debug: true` | `-g` | compile `/Z7`, link `/DEBUG` |
| `assertions: false` | `-DNDEBUG` | `/DNDEBUG` |
| `lto: true` | compile and link `-flto` (`-flto=auto` on GCC); archiver `gcc-ar`/`llvm-ar` | compile `/GL`, link `/LTCG` |
| `sanitizers` | compile and link `-fsanitize=<list> -fno-omit-frame-pointer` | `address` → `/fsanitize=address`, whose runtime DLL is found through the environment of §1.5; others warn and are dropped |
| `runtime: static` | link: Linux/macOS `-static-libstdc++ -static-libgcc`; MinGW `-static` | `/MT` instead of `/MD` |
| `debugRuntime: true` | — | `/MDd` or `/MTd` |
| `warnings: none` | `-w` | `/W0` |
| `warnings: default` | `-Wall -Wextra` | `/W4` |
| `warnings: strict` | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`, plus `-Wnon-virtual-dtor -Woverloaded-virtual` for C++ | `/W4`, plus the warnings that `/W4` leaves off and GCC's set has: conversions that lose data or change sign (`/w14242 /w14254 /w14365 /w14826`), a class with virtual functions and a non-virtual destructor (`/w14265`), a virtual function hidden by an overload (`/w14263`), and suspicious comparisons and casts (`/w14287 /w14296 /w14905 /w14906 /w14928`) |
| `werror: true` | `-Werror` | `/WX` |
| PIC (dependency builds, shared libraries; not on Windows) | `-fPIC` | — |
| always | `-fdiagnostics-color=always` (see the note below) | `/nologo /utf-8 /Zc:preprocessor /bigobj /external:W0`, and for C++ `/EHsc /Zc:__cplusplus /permissive-`. `/external:W0` silences the toolchain's own headers (§1.5). MSVC has no colour flag |
| `cflags` / `cxxflags` / `ldflags` | appended last, verbatim: `cflags` to C compiles, `cxxflags` to C++ compiles | appended last, verbatim |

The rest of a profile applies to C as it does to C++: `opt`, `debug`, `assertions`, `lto`, `sanitizers`, `warnings`, `werror`, `defines`, PIC and the colour flag. C and C++ translation units also get the same include directories and definitions (§4.3).

The colour flag is always passed, so that changing the colour setting never changes command lines and never triggers a rebuild. Ninja strips colour codes itself when its output is not a terminal. CPM makes Ninja follow its own colour decision ([cli.md §1](cli.md#1-global-options)) by setting `CLICOLOR_FORCE=1` when colour is on and `TERM=dumb` when it is off.

Unsupported combinations warn once and are dropped, never silently changed. Examples: sanitizers on MinGW, `memory` with GCC, `thread` together with `address`, `undefined` with MSVC.

With MSVC, `debug: true` puts the debug information in the object files (`/Z7`) rather than in a shared `.pdb`, so parallel compiles never compete for one file. The link then writes the program's `.pdb` next to it.

## 4. Build plan and Ninja generation

### 4.1 Build and output directories

Intermediates live under `cpm_modules/` and outputs in a top-level `build/`, so the files people use are easy to find, and deleting `cpm_modules/` (to reinstall dependencies) or `build/` (to clean outputs) never touches the other's job.

```
cpm_modules/.cache/<profile>/                   # intermediates
  build.ninja   .ninja_log   .ninja_deps   archives.json
  obj/<path relative to project root>.o         # obj/src/cli/args.cpp.o, obj/tests/json.test.cpp.o
  obj/deps/<name>/<path relative to dep root>.o # path dependencies
  lib/deps/lib<name>.a                          # path dependencies

build/<profile>/                                # outputs
  <name>[.exe]                                  # executable
  lib<name>.a                                   # static library
  lib<name>.so | lib<name>.dylib | lib<name>.dll (+ lib<name>.dll.a import library on MinGW)
  tests/<stem>[.exe]                            # tests/json.test.exe
```

Each profile has its own `build.ninja` and output directory, so switching profiles keeps both object trees and Ninja logs warm. CPM writes only inside `build/<profile>/`; other files in `build/` are left alone. `cpm init` adds `/build/` (anchored to the package root, so a source directory such as `src/build/` stays tracked) to `.gitignore`.

### 4.2 Targets

| Ninja target | Contents | Built by |
|---|---|---|
| `<name>` (phony) | the root's main output (executable, static library or shared library) | `cpm build` |
| `all` (phony, **default**) | `<name>` | `cpm build` |
| `tests` (phony) | every test executable | `cpm test`, `cpm build --tests` |
| `<stem>` (phony) | one test executable, e.g. `json.test` | `cpm test json` |

### 4.3 Include directories, definitions and link order

**Dependency order:** a topological order of the resolved graph in which every package comes **before** its dependencies. Ties are broken by name. Call it *D*. Dev-only packages are appended at the end and are used only by test targets.

- **Include directories** (`incs`): `src/`, the root's `include/` (if it exists), then `cpm_modules/<pkg>/include` for each `pkg` in *D*. Test translation units add dev-dependency includes and `tests/`.
- **Definitions:** profile `defines` and `assertions`, then the root's `defines` and `publicDefines`, then the `publicDefines` of every package in *D*, then platform-override additions. Duplicates are removed (first occurrence kept). Conflicting values for the same name are an error. Last come the export macros ([manifest.md §4.1](manifest.md#41-exported-symbols-name_api)): the root's own `<NAME>_API`, and that of every library package in *D*, empty.
- **Link line** for an executable, a shared library or a test: the target's own objects (core objects plus entrypoint, or the root library), then for each library package in *D*: `-Lcpm_modules/<pkg>/lib -l<linkName>`, path-dependency archives, then `systemLibs` of the root and of every package in *D*, deduplicated **keeping the last occurrence**, then the profile `ldflags`. Store archives are also listed as implicit inputs (`| …/libfmt.a`), so a rebuilt dependency triggers a relink.

### 4.4 Path dependencies

Path packages are compiled inside the consumer's graph rather than in a sandbox, so edits to them rebuild incrementally.

- Their translation units use **their own** include directories (their `src/`, their `include`, and their dependencies' includes), their own private `defines`, their own `cstd` for C, and the root profile's flags. These are per-edge variable overrides in Ninja.
- They are archived into `lib/deps/lib<name>.a` and linked like store libraries.
- `cpm_modules/<name>/include` links to the package's include directory, so consumer code sees a uniform layout.

### 4.5 `build.ninja` format

```ninja
# Generated by cpm 0.1.0 for hello@0.1.0 (profile: debug, abi: 3f9a1c2b7d4e5a60). Do not edit.
ninja_required_version = 1.10
builddir = cpm_modules/.cache/debug

cxx = C$:/Strawberry/c/bin/g++.exe
ar = C$:/Strawberry/c/bin/gcc-ar.exe
cxxflags = -std=c++23 -O0 -g -Wall -Wextra -fdiagnostics-color=always
defines =
incs = -Isrc -Iinclude -Icpm_modules/fmt/include
ldflags =

rule cxx
  command = $cxx -MD -MF $out.d $cxxflags $defines $incs -c $in -o $out
  depfile = $out.d
  deps = gcc
  description = CXX $in
rule ar
  command = $ar rcs $out @$out.rsp
  rspfile = $out.rsp
  rspfile_content = $in
  description = AR $out
rule link
  command = $cxx @$out.rsp -o $out $ldflags
  rspfile = $out.rsp
  rspfile_content = $in $libs
  description = LINK $out

build cpm_modules/.cache/debug/obj/src/greeting.cpp.o: cxx src/greeting.cpp
build cpm_modules/.cache/debug/obj/src/main.cpp.o: cxx src/main.cpp
build build/debug/hello.exe: link $
    cpm_modules/.cache/debug/obj/src/main.cpp.o $
    cpm_modules/.cache/debug/obj/src/greeting.cpp.o $
    | cpm_modules/fmt/lib/libfmt.a
  libs = -Lcpm_modules/fmt/lib -lfmt

build cpm_modules/.cache/debug/obj/tests/greeting.test.cpp.o: cxx tests/greeting.test.cpp
  incs = $incs -Itests
build build/debug/tests/greeting.test.exe: link $
    cpm_modules/.cache/debug/obj/tests/greeting.test.cpp.o $
    cpm_modules/.cache/debug/obj/src/greeting.cpp.o $
    | cpm_modules/fmt/lib/libfmt.a
  libs = -Lcpm_modules/fmt/lib -lfmt

build hello: phony build/debug/hello.exe
build greeting.test: phony build/debug/tests/greeting.test.exe
build all: phony hello
build tests: phony greeting.test
default all
```

**C sources.** When the build compiles C, the file also defines `cc` and `cflags`, and a `cc` rule that the C translation units use. The example's project with a `src/parse.c` added would get:

```ninja
cc = C$:/Strawberry/c/bin/gcc.exe
cflags = -std=gnu17 -O0 -g -Wall -Wextra -fdiagnostics-color=always

rule cc
  command = $cc -MD -MF $out.d $cflags $defines $incs -c $in -o $out
  depfile = $out.d
  deps = gcc
  description = CC $in

build cpm_modules/.cache/debug/obj/src/parse.c.o: cc src/parse.c
```

A path dependency's C translation units override `cflags` per edge, with their own `cstd`. Links still use `$cxx` (§1.2). A build with no C sources has none of these lines.

Rules:

- **Determinism:** edges are emitted in a fixed order: compile edges sorted by source path, then archive, link and phony edges in target order. The same inputs give a byte-identical file.
- **Paths** use `/`. Paths inside the project root are relative to it, because Ninja runs with the project root as its working directory. Paths outside it are relative (`../mylib/src/x.cpp`) when they are on the same volume, and absolute otherwise.
- **Escaping:** in `build` lines, `$` → `$$`, space → `$ `, `:` → `$:`. In variable values, `$` → `$$`. Newlines are never allowed. Arguments that contain spaces are quoted for the command interpreter: MSVC-CRT argv quoting on Windows (Ninja calls `CreateProcess` directly), single quotes on POSIX (`/bin/sh -c`).
- **Response files** are used for every link and archive step, which avoids the 32 KiB command-line limit on Windows.
- **Dependency stamp:** with dependencies, CPM writes `cpm_modules/.cache/<profile>/dependencies.stamp` (only when its content changes) listing every dependency's version and identity (integrity, git commit or path) and the ABI, and every compile and link edge depends on it. `cpm_modules` links can point at different files under the same paths, possibly with older timestamps, which Ninja alone would not notice; the stamp makes any dependency change rebuild the dependents.
- **macOS archives:** the BSD `ar` there does not read `@file` response files, so on macOS the `ar` rule passes the objects directly: `$ar rcs $out $in`.
- **Stale archive members:** `ar rcs` never removes members. CPM records each archive's member list in `archives.json`. When the list changes, it deletes the archive before running Ninja, so the archive is rebuilt from scratch.
- **No self-regeneration rule.** CPM always regenerates before invoking Ninja, so Ninja never needs to rerun CPM.

### 4.6 `compile_commands.json`

After generating `build.ninja`, CPM runs `ninja -f <file> -t compdb cxx cc` and writes the result to `<project>/compile_commands.json`, only if its content changed. Without this file, clangd and IDEs could not find dependency headers.

### 4.7 Shared libraries at run time (*post-MVP for shared-library dependencies*)

- A root `shared-lib` target is supported in the MVP: `-shared`, plus `-fPIC` off Windows. On MinGW it adds `-Wl,--out-implib,<lib>.dll.a`, and every symbol is exported automatically when the code has no `__declspec(dllexport)`. On Linux it adds `-Wl,-soname,lib<name>.so`, on macOS `-dynamiclib -Wl,-install_name,@rpath/lib<name>.dylib`, so dependents record the file name rather than the build path. The package's tests depend on the library and find it through `-Wl,-rpath,$ORIGIN/..` (macOS: `@loader_path/..`).
- **With MSVC,** a root `shared-lib` links with `/DLL /IMPLIB:<name>.lib`, which writes `<name>.dll` and its import library `<name>.lib` into `build/<profile>/`. The tests link the import library and find the DLL at run time through `PATH`, which `cpm test` starts with `build/<profile>/` (§7).
  - What the library exports is what it marks with its `<NAME>_API` ([manifest.md §4.1](manifest.md#41-exported-symbols-name_api)).
  - When it marks nothing, `link.exe` writes no import library and the build fails. CPM then says so: `the shared library mylib exports nothing: mark its API with MYLIB_API (manifest.md §4.1)`.
- For shared-library **dependencies**, CPM copies their runtime files into `build/<profile>/` after linking on Windows (there is no rpath). On ELF and Mach-O it adds `-Wl,-rpath,<absolute store lib dir>`.

## 5. Dependency builds

When the store has no `lib/<abi16>/` for a registry or git package and the registry offers no matching prebuilt binary, CPM builds it in a **sandbox**:

1. Take the entry's lock (`<entry>/lib/<abi16>`, [store.md §4](store.md#4-atomicity-and-locking)), then check again for a complete directory.
2. Create `$CPM_HOME/tmp/build-<random>/`.
3. Plan the package as a library ([manifest.md §4](manifest.md#4-target-inference), with its overlay applied):
   - sources are absolute paths into the store entry, which is read-only
   - include directories: its `src/`, its include directory, and the include directories of its resolved dependencies **in the store** (never the consumer's `cpm_modules/`)
   - definitions: its `defines` and `publicDefines`, its dependencies' `publicDefines`, and its platform overrides
4. **Flags:** a synthetic profile `{ opt: 2, debug: false, assertions: false, lto: false, warnings: "none" }` combined with every setting that the ABI fingerprint encodes: root `std`, `runtime`, `debugRuntime`, `sanitizers`, `defines.abi`, the exceptions and RTTI flags, and PIC. Artifacts therefore always match their ABI directory. The package's C sources are compiled with its own `cstd` ([manifest.md §3.3](manifest.md#33-cstd-semantics)) and the same synthetic profile.
5. Write `build.ninja` into the sandbox and run Ninja with output captured to `$CPM_HOME/logs/build-<name>@<version>-<abi16>.log`. `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH` and `LIBRARY_PATH` are cleared for the build, as are `INCLUDE` and `LIB` for non-MSVC toolchains, so headers and libraries from the environment cannot leak in.
6. Copy the artifacts into a staging `lib/<abi16>.tmp-<random>/`, write `meta.json` and `.complete`, then rename to `lib/<abi16>/`.
7. Remove the sandbox, unless the build failed and `CPM_KEEP_SANDBOX=1` is set.

Dependencies are built in dependency order, and a failure stops the install. Header-only packages are never built. *Post-MVP:* independent dependencies build in parallel, limited by `-j`.

## 6. Running Ninja

- **Discovery:** `CPM_NINJA`, then `PATH`, then `$CPM_HOME/tools/ninja[.exe]`, then on Windows the Ninja that Visual Studio installs with its CMake tools (`Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe` in an installation from §1.4). If none is found, the error includes install hints. *Post-MVP:* CPM downloads a pinned Ninja release into `tools/`.
- **Invocation:** `ninja -f cpm_modules/.cache/<profile>/build.ninja [-j N] [-v] <targets>`, with the project root as working directory. `NINJA_STATUS` is set to `[%f/%t] `, and the toolchain environment (MSVC) is applied.
- **Output:** streamed straight through. CPM adds a header line before it and a summary line after it ([cli.md §4](cli.md#4-output)).
- **Exit status:** if Ninja exits non-zero, CPM exits with 1 after the compiler output has been shown. A failed build never changes `.cpm-modules.json` or the lockfile.
- **Replacing the running executable (Windows):** before Ninja runs, if any output path of the requested targets resolves to the running CPM executable, CPM renames that file to `<file>.old`, which Windows allows. At startup CPM deletes any `<own path>.old` it can.

## 7. Tests

- **Discovery:** `tests/*.test.{cpp,cc,cxx,c}`, one executable per file ([manifest.md §4](manifest.md#4-target-inference)).
- **Build:** `cpm test [filter…]` builds the matching `<stem>` targets, or `tests` when no filter is given.
- **Run:** tests run one at a time, sorted by name. For each test:
  - working directory: the project root
  - stdin: closed
  - environment: adds `CPM_TEST=1`, `CPM_PROFILE`, `CPM_PROJECT_ROOT`
  - timeout: 300 s by default (`--timeout <s>`)
- **Result:** exit code `0` = pass, `77` = skipped (Automake convention), anything else (or a timeout) = fail.
- **Output:** captured. It is shown for failing tests, or for every test with `-v`.
- **Summary and exit status:** a summary line such as `5 passed, 1 skipped, 1 failed in 0.84s` lists the failing tests by name. `cpm test` exits with 1 if any test failed.

## 8. Legacy Bridge (*post-MVP*)

Some packages can only be built with their own build system. An overlay can request a bridged build:

```json
"manifest": {
  "type": "static-lib",
  "build": { "system": "cmake", "options": { "FMT_TEST": "OFF", "FMT_DOC": "OFF" }, "link": ["fmt"] }
}
```

- **CMake:** the sandbox runs
  `cmake -S <entry> -B <sandbox>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=<cxx> -DCMAKE_CXX_STANDARD=<n> -DCMAKE_CXX_FLAGS=<ABI flags> -DCMAKE_POSITION_INDEPENDENT_CODE=<pic> -DBUILD_SHARED_LIBS=<OFF|ON> -DCMAKE_INSTALL_PREFIX=<sandbox>/prefix -D<options>…`,
  then `cmake --build`, then `cmake --install`.
- **Meson:** `meson setup --buildtype=release --default-library=static -Dcpp_std=<std> -Db_staticpic=<pic>`, then `meson install` into the prefix.
- **Harvest:** `prefix/include/` goes to `lib/<abi16>/include/`. Generated headers can depend on the configuration, so bridged packages export **this** directory as their include directory. The libraries named by `link` (default: every archive in `prefix/lib/`) go to `lib/<abi16>/`. A `meta.json` is written as usual.
- The bridge toolchain must be the same compiler as the consumer. CMake and Meson are looked up on `PATH`. A missing tool is an error with an install hint.
