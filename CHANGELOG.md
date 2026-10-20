# Changelog

Every CPM release, newest first. A version's section here becomes its release notes on GitHub: the [release workflow](.github/workflows/release.yml) publishes it, followed by how to install. It refuses to release a version that has no section. See [CONTRIBUTING.md](CONTRIBUTING.md#releasing).

## 0.4.3 — 2026-10-20

When you know a dependency works with a version its range doesn't allow, say so in your project.

### New

- **Version overrides.** `"overrides"` in your project's `cpm.jsonc` replaces the range of your dependencies' requirements, so you don't have to wait for a dependency to release:

  ```jsonc
  "overrides": {
    "fmt": "^10.2.0",         // every package's requirement on fmt
    "spdlog>fmt": "^10.2.0"   // only spdlog's requirement on fmt
  }
  ```

  The scoped form wins over the plain one. There is still one fmt, and your project's own ranges in `dependencies` and `devDependencies` are left as you wrote them.
- **Overrides are never silent.**
  - `cpm install` lists every requirement an override replaced (`Overriding spdlog 1.12.0's requirement fmt ^9.1.0 with ^10.2.0`), and warns about an override that replaces nothing.
  - `cpm-lock.json` records the overrides, and which requirements they replaced, with the dependency's own range.
  - `cpm why` shows `^10.2.0 (override of ^9.1.0) from spdlog 1.12.0`.
  - `cpm outdated` notes an override that is no longer needed, because the dependency now accepts the version you use by itself.
  - Conflict explanations mark overridden requirements, and their hint names the override.
- **Only your project overrides.** `cpm publish` refuses a package that declares overrides, and overrides in a git or path dependency's `cpm.jsonc` are ignored with a warning.

## 0.4.2 — 2026-10-20

When versions can't fit together, CPM now says why.

### New

- **A new resolver.** CPM picks versions with PubGrub, the algorithm behind Dart's pub and uv. It learns from each conflict instead of retrying combinations that are known to fail, so it stays fast on large dependency graphs, and it finds a solution whenever one exists.
- **Conflicts are explained** as a chain of reasons that ends at your project, followed by a hint that names the ranges in your `cpm.jsonc` that are involved:

  ```
  error: the project's dependencies can't be resolved
    Because spdlog >=1.12.0 depends on fmt ^9.1.0 and the project depends on fmt ^10.2.0, spdlog >=1.12.0 can't be used.
    So, because the project depends on spdlog ^1.12.0, no versions satisfy every requirement.
    hint: change the range of fmt or spdlog in cpm.jsonc; cpm info <package> shows what each version depends on
  ```

  A range that matches nothing says which versions exist, and a path or git package says which version it has.

### Changed

- A package version that depends on a package its registry doesn't have is now skipped, and CPM uses another version. Before, installing failed.
- Results don't change otherwise: the versions in `cpm-lock.json` are kept while they still fit, and the same inputs always give the same result. With no lockfile, CPM may now choose a different set of versions than 0.4.1 would have, when more than one set works.

## 0.4.1 — 2026-10-20

See what you depend on and why, and keep `~/.cpm` tidy.

### New

- **`cpm info <package>[@<range>]`** shows a package on the registry without installing it: its type, C++ standard, license and description, its repository, owners and downloads, the dependencies of the version shown, and its versions with yanked ones marked. With a range it shows the version `cpm install` would pick. `--registry` asks another registry; for a static registry it shows what the index says.
- **`cpm tree`** prints the project's resolved dependencies from `cpm-lock.json` as a tree, with devDependencies and git and path sources marked. `--depth <n>` limits how deep it goes and `--prod` leaves out devDependencies.
- **`cpm why <package>`** shows every chain of dependencies that brings a package into the project, with the range each step asks for. It helps before you change a range, or when two dependencies want different versions.
- **`cpm cache`** shows what CPM keeps in `~/.cpm`: the packages in the store, the libraries built there, downloads, git mirrors, the projects CPM knows, and how much of it nothing uses. **`cpm cache clean`** removes what no project uses (`--dry-run` shows it first). `cpm cache clean --all` empties the store and caches, and keeps your configuration, sign-ins and the tools `cpm setup` installed.

### Also

- `cpm install` and `cpm build` now record each project in `~/.cpm/projects.json`, which is how `cpm cache clean` knows what is still in use. A project that hasn't run either command since you upgraded isn't known yet: run `cpm install` in it before cleaning. Otherwise what it uses may be removed, and its next build downloads and builds it again.

## 0.4.0 — 2026-10-19

Find packages and stale dependencies without leaving the terminal.

### New

- **`cpm search <query>`** searches the registry by name and description, and lists each match with its latest version and description. `--registry` searches another registry service. A static registry, which has no search, says so.
- **`cpm outdated`** lists the registry dependencies whose locked version (Current), the newest version their range allows (Wanted) and the newest version (Latest) differ, like `npm outdated`. Yanked versions never count, and releases come before prereleases. Path and git dependencies are skipped with a note.
- **CPM has a logo**: a "c" with the "++" of C++. It's on the README, cpm-registry.com and the favicon.

### Also

- The one-line installers are now tested on fresh Linux, macOS and Windows machines every week and after each release. They install CPM, set up a compiler, then build and test a project that uses a registry package. This is the first time `cpm setup` runs on macOS in CI.
- The registry protocol ([registry.md §7.5](docs/spec/registry.md#75-browsing)) documents what a registry service offers for browsing: search filters, sorting and facets, download statistics, dependents, publisher profiles, totals and README badges. Clients ignore what they don't use.

## 0.3.0 — 2026-10-18

One command takes a machine from nothing to building C++.

### New

- **One-line installers.** `curl -fsSL https://cpm-registry.com/install.sh | sh` on Linux and macOS, and `irm https://cpm-registry.com/install.ps1 | iex` in PowerShell. They download the latest CPM, check its SHA-256, put it on your `PATH`, and run `cpm setup`. Running one again updates CPM. `CPM_HOME`, `CPM_NO_SETUP=1` and `CPM_NO_MODIFY_PATH=1` change what they do.
- **`cpm setup`** finds the compiler, Ninja, git, curl and tar, says what it will change, asks, and installs what is missing:
  - Windows: WinLibs GCC and Ninja, into `~/.cpm/tools`, with nothing outside your user folder;
  - Linux: the distribution's packages through apt-get, dnf, pacman, zypper or apk, choosing the newest GCC the distribution has;
  - macOS: Apple's Command Line Tools, and Ninja into `~/.cpm/tools`.

  Downloads are pinned with checksums. `--check` only reports, `--yes` doesn't ask, and `--managed` installs CPM's own tools even when others work.

### Fixed

- On Windows, `cpm run` and `cpm test` put the compiler's folder on `PATH`. Programs built with a compiler that isn't on `PATH`, such as WinLibs or MSYS2 GCC, now find libstdc++ and the other runtime DLLs.

## 0.2.0 — 2026-10-17

A public registry, and publishing to it.

### New

- **cpm-registry.com is the default registry.** `cpm install fmt` works with no configuration. The registry starts with fmt, spdlog, nlohmann-json, doctest, Catch2 and CLI11, and its website lists every package.
- **`cpm publish`** packs a library into an archive that is byte-identical on every platform, checks it, and publishes it to a registry service or a static registry. `--dry-run` checks and packs without publishing; `--out` writes the files to upload elsewhere.
- **Accounts.** `cpm login` signs in with GitHub, and `cpm login --with-token` takes an API token for CI. `cpm logout` and `cpm whoami` complete the set. Tokens are kept in `$CPM_HOME/credentials.json`, never in the configuration.
- **`cpm yank`** hides a published version from new installs (`--undo` brings it back). **`cpm owner`** lists, adds and removes who may publish a package.
- **`cpm update`** (alias `up`) moves dependencies to the newest versions their ranges allow, and git branches to their newest commit.

### Fixed

- A git mirror or store entry damaged from outside CPM, for example by a cleaner of temporary files, is now rebuilt instead of failing the install or the build.
- Releases include the bare Windows executable as well as the zip.

## 0.1.0 — 2026-10-16

The first release: the npm workflow, for C++.

- **One manifest, a lockfile.** `cpm.jsonc` describes the project, and `cpm-lock.json` pins every version, git commit and archive hash. CPM edits the manifest in place and keeps its comments.
- **`cpm init`** makes an executable, a static or shared library, or a header-only library, with a test. **`cpm build`**, **`cpm run`** and **`cpm test`** build with Ninja directly: no CMake.
- **`cpm install`** and **`cpm remove`** handle registry, git and path dependencies. Every archive is checked against its SHA-256. `--offline`, `--prefer-offline` and `--no-build` control the network and building.
- **Built once, shared.** Each dependency is built once per ABI fingerprint into a global store, `~/.cpm`, and linked into each project's `cpm_modules/`. A registry's prebuilt binaries are used when they match.
- **GCC and Clang** on Windows (MinGW-w64), Linux and macOS, with profiles, sanitizers and per-platform settings.

CPM is plain C++23 with no third-party code, and it builds itself. Binaries are for Windows (x86-64), Linux (x86-64, statically linked, any distribution) and macOS (Apple silicon).
