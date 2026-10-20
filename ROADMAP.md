# Roadmap to CPM 0.5.0

**0.5.0: CPM handles real projects on every major toolchain.** You can pick the compiler, including MSVC; depend on the C and C++ libraries projects actually use, prebuilt where possible; understand and fix conflicts in your dependencies; keep several packages in one repository; and inspect what you depend on.

We get there in small releases, 0.4.1, 0.4.2 and so on. Each one is useful on its own and ships with notes in [CHANGELOG.md](CHANGELOG.md). Each lands in the same way: spec first ([docs/spec](docs/spec/README.md)), then code and tests, CI green on every platform, a changelog section, a tag, and the [installer check](.github/workflows/installer.yml) installing the new version on fresh machines.

| Release | Theme | In short |
|---|---|---|
| 0.4.1 | Everyday commands | `cpm info`, `cpm tree`, `cpm why`, `cpm cache` |
| 0.4.2 | Clearer conflicts | A new resolver that explains why versions can't fit together |
| 0.4.3 | Version overrides | Force a version when you know a dependency works with it |
| 0.4.4 | Choose the compiler | List compilers; pick one per machine, project or command |
| 0.4.5 | C sources | Compile `.c` files, so C libraries can be packaged |
| 0.4.6 | More libraries | About 30 packages on cpm-registry.com, C libraries included |
| 0.4.7 | Prebuilt binaries | Installs download built libraries instead of compiling them |
| 0.4.8 | MSVC, part 1 | Build and test with Visual Studio's compiler |
| 0.4.9 | MSVC, part 2 | Shared libraries, dependencies and prebuilt binaries with MSVC |
| 0.4.10 | Symbol isolation | Let a dependency keep its own copy of a library, without clashes |
| 0.4.11 | Workspaces, part 1 | Several packages in one repository, one lockfile |
| 0.4.12 | Workspaces, part 2 | Choosing members, scripts and publishing from a workspace |
| 0.5.0 | Ready | A user guide, stable formats, polish |

The order follows dependencies:
- The first three releases are about understanding and fixing your dependency graph, and they build on each other. `cpm why` shows who needs what, the new resolver explains conflicts, and overrides are built on that resolver.
- Choosing the compiler comes before MSVC, which is then one more compiler to choose.
- C sources come before the catalog grows into C libraries, and prebuilt binaries before MSVC, so MSVC's binaries ride on them.
- Symbol isolation comes after MSVC, because on MSVC it needs the shared-library support from MSVC part 2.
- Workspaces stand alone and could move earlier.

Independent pieces may be built in parallel, but releases go out in this order unless we decide otherwise.

## One version of each library

The next three sections all build on one rule, which helps to know first. CPM gives a project **one version of each library** ([dependencies.md §4.1](docs/spec/dependencies.md#41-model)). When your project and a dependency both use fmt, they share one fmt that satisfies both of their ranges. When no version satisfies them all, installing fails with an error that names the conflicting constraints.

The reason is C++'s One Definition Rule. Two versions of a library linked into one program define the same functions and classes. That either fails to link or, worse, silently mixes one version's code with the other's data layouts. The rule stays; 0.4.2, 0.4.3 and 0.4.10 make conflicts easier to understand, to resolve, and in narrow cases to avoid.

## 0.4.1: Everyday commands

Small tools for understanding a project and its dependencies.

- `cpm info <package>[@<range>]` shows a package from the registry: its description, license, repository, versions (yanked ones marked), and dependencies.
- `cpm tree` prints the resolved dependency graph from `cpm-lock.json`. `cpm why <package>` shows the paths that pull a package in.
- `cpm cache` shows what the store in `~/.cpm` holds and how much space it takes. `cpm cache clean` removes what no project uses any more, and `cpm cache clean --all` empties it.

Done when all four are specified in cli.md and tested, including against a registry service and a `file://` registry.

## 0.4.2: Clearer conflicts

Replace the backtracking resolver with PubGrub, as [dependencies.md §4.4](docs/spec/dependencies.md#44-pubgrub-post-mvp) already plans.

- It finds a solution whenever one exists, and stays fast on large graphs, where backtracking can try many dead ends.
- When no solution exists, it explains why as a chain of reasons. For example: "spdlog 1.12 and later need fmt ^9, and your project needs fmt ^10, so no spdlog from 1.12 on can be used. Your project needs spdlog ^1.12, so these requirements can't be met." It then suggests ways out: relaxing a range, an older version of a dependency, or an override (0.4.3).
- Results don't change otherwise. Locked versions are kept, the same inputs give the same result, and where today's resolver finds a solution with a lockfile, the new one finds the same.

Done when the existing resolver tests pass unchanged, new tests cover conflict explanations with the exact text users see, and resolving large generated graphs stays fast.

## 0.4.3: Version overrides

When you know a dependency works with a version its range doesn't allow, say so in your project instead of waiting for the dependency to release.

- `"overrides": { "fmt": "^10.2" }` in the project's `cpm.jsonc` makes every dependent accept that range for fmt, whatever its own range says. A scoped form, such as `"spdlog>fmt": "^10.2"`, relaxes only spdlog's requirement. There is still one fmt.
- Overrides are never silent. The lockfile records which requirements were overridden, `cpm install` and `cpm why` show them, and `cpm outdated` notes when an override is no longer needed.
- Only the project being built can override. Libraries can't, since an override is the final project's decision: `cpm publish` refuses a package that declares overrides, and overrides in dependencies are ignored with a warning.

Done when overrides are specified in dependencies.md, manifest.md and lockfile.md. The fmt 9 against fmt 10 example must fail without an override and build with one.

## 0.4.4: Choose the compiler

CPM picks a compiler today from `CXX`, `--cxx` or `config.jsonc`, else the first one on `PATH`. This release makes the choice visible and deliberate.

- `cpm toolchain list` shows every compiler CPM can find, with its version and whether CPM can use it. That covers `PATH`, versioned compilers (`g++-14`, `clang++-18`), MSYS2 and WinLibs installs, and `~/.cpm/tools`.
- Choose a compiler per machine (`cpm toolchain use clang`, saved in `config.jsonc`), per project (`"toolchain": "gcc >=14"` in `cpm.jsonc`), or per command (`--toolchain`). The order is documented: command, then `CXX`, then project, then machine, then automatic. A project that asks for a compiler the machine lacks gets a clear error saying how to install it.
- `cpm setup --compiler gcc|clang` installs a particular compiler where `cpm setup` can.
- Every build says which compiler it used and why it was chosen.

Done when the choice order is specified in build.md and cli.md, and tested with two installed compilers (CI has GCC and Clang on Linux).

## 0.4.5: C sources

`.c` files are compiled with the C compiler that matches the chosen C++ compiler. A `"cstd"` field sets the C standard. Profiles, defines and include paths apply to C as well, and ABI fingerprints account for the C compiler. Mixed C and C++ targets link correctly, and C libraries can be published and installed like any other.

Done when a pure C library, a mixed C/C++ program and a C++ program that depends on a C library build and test on every platform, and zlib builds from a `cpm.jsonc`.

## 0.4.6: More libraries

Grow cpm-registry.com from 6 packages to about 30, chosen by what C++ projects use most:

- C libraries: zlib, sqlite, libpng, zstd, lz4;
- C++ libraries: magic_enum, glm, Eigen, toml++, cxxopts, range-v3, GoogleTest, Google Benchmark, and more.

Each gets a seed recipe and a smoke test (cpm-registry's `seed/`). Whatever CPM is missing to package them gets fixed in CPM, which is why this is a release and not only registry work.

Done when every package installs and runs its smoke test from an empty CPM home, on Linux, macOS and Windows.

## 0.4.7: Prebuilt binaries

The store and the protocol already accept prebuilt binaries ([registry.md §4.2](docs/spec/registry.md#42-binary-archives)), but no registry serves any.

- `cpm publish --with-binary` adds a binary built with the publisher's toolchain, tagged with its ABI fingerprint.
- The registry service stores and serves binary archives next to source archives, within its storage limits.
- A cpm-registry workflow builds binaries of popular packages for the common toolchains: GCC on Linux, Apple Clang on macOS and MinGW GCC on Windows.
- `cpm install` uses a matching binary, and builds from source otherwise, as it does today.

Done when installing fmt and spdlog on a common toolchain downloads binaries and builds nothing, and every other toolchain still builds from source.

## 0.4.8: MSVC, part 1

Visual Studio's compiler, `cl.exe`, as one more toolchain.

- Find Visual Studio and its Build Tools with `vswhere`, and set up their environment without a Developer Command Prompt.
- An MSVC driver: profile flags, warnings, defines, include paths and C++ standards mapped to `cl.exe`. Static libraries are built with `lib.exe` and programs linked with `link.exe`. `cpm test` works.
- ABI fingerprints cover the toolset version and the runtime library (`/MD` or `/MT`).
- `cpm toolchain list` shows MSVC, and `"toolchain": "msvc"` selects it.

Done when CPM's own tests pass when CPM is built with MSVC, and CI runs a Windows MSVC job.

## 0.4.9: MSVC, part 2

- Shared libraries: symbol exports (`__declspec(dllexport)` through a generated export macro), import libraries, and DLLs found at run time.
- Dependencies from the registry build with MSVC, and C sources compile with `cl.exe`.
- `cpm setup` on Windows can install Visual Studio Build Tools (through winget, after asking), as an alternative to WinLibs GCC.
- Prebuilt binaries for MSVC from the cpm-registry workflow.

Done when the catalog's packages install and pass their smoke tests with MSVC.

## 0.4.10: Symbol isolation

Sometimes two dependencies need incompatible versions of a library that each uses only internally. `"isolate": true` on one of them lets it keep its own copy ([dependencies.md §5](docs/spec/dependencies.md#5-odr-policy-and-symbol-isolation)).

- The isolated dependency and its private dependencies are resolved separately. They're compiled with hidden symbols and combined into one library whose private symbols can't clash with anything else in the program.
- CPM refuses isolation when the dependency's public headers expose types from its private copy, since passing those types across would break the One Definition Rule after all. The error names the header.
- Each platform hides symbols its own way: `objcopy` with GNU toolchains (Linux and MinGW), the linker's exported-symbols list on macOS, and a DLL on MSVC, using 0.4.9's shared libraries.
- `cpm tree` and `cpm why` show isolated copies.

Done when a project depending on two libraries that use fmt 9 and fmt 10 internally builds, runs and passes its tests on every platform, MSVC included, and the exposed-type case is refused.

## 0.4.11: Workspaces, part 1

Several packages in one repository.

- A workspace root `cpm.jsonc` lists its members (`"workspace": ["packages/*"]`).
- Members depend on each other by name, with no version ranges or path specifiers needed. There is one `cpm-lock.json` and one `cpm_modules/` resolution for the whole workspace.
- `cpm build` and `cpm test` at the root build and test every member, in dependency order.

Done when a workspace with a library, a program using it and tests builds and tests on every platform, and resolves to a single lockfile.

## 0.4.12: Workspaces, part 2

- `--package <name>` (`-p`) builds, tests or runs selected members. Commands run inside a member's folder act on that member.
- Scripts run per member, or across the workspace.
- `cpm publish` publishes a member. Dependencies on other members become version ranges on their published versions.
- `cpm install <package>` inside a member adds the dependency to that member.

Done when the workspace from 0.4.11 publishes its library and installs it into an outside project.

## 0.5.0: Ready

- **A user guide:** task-based documentation for people using CPM, separate from the specification. It covers getting started, adding dependencies, fixing conflicts, choosing compilers, publishing, workspaces, and CI recipes. It's on the website.
- **Stable formats:** `cpm.jsonc`, `cpm-lock.json` and the registry protocol are declared stable. From then on, changes stay backward compatible or come with a migration.
- **Polish:** error messages, `--help` text and the README, reviewed with everything above in place.

Done when the guide is published, and every command and format change since 0.4.0 is in the spec and the changelog.

## Later

Building CMake-based libraries directly (the "Legacy Bridge"), cross-compilation, and C++ modules (`import`).
