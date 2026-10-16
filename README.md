# CPM — C++ Package Manager

[![CI](https://github.com/meirborowski/CPM/actions/workflows/ci.yml/badge.svg)](https://github.com/meirborowski/CPM/actions/workflows/ci.yml)

CPM brings the npm/pnpm workflow to C++: one declarative manifest (`cpm.jsonc`), a deterministic lockfile, a project-local `cpm_modules/`, a global ABI-keyed binary cache, and direct Ninja generation. No CMake.

> **Status: early and experimental (v0.1).** `cpm init`, `install`, `remove`, `build`, `run` and `test` work with GCC and Clang, for registry, git and path dependencies. So far CPM has only been tested on Windows with MinGW-w64 GCC 13. The Linux and macOS code paths exist but have not been run yet. MSVC is not supported yet, and there is no public package registry yet (see [Registries](#registries)).

## Why CPM

- **One manifest, no CMake.** A `cpm.jsonc` and a conventional layout are the whole build description. CPM writes Ninja files directly.
- **Reproducible installs.** `cpm-lock.json` pins every version, git commit and archive hash.
- **Build once, reuse everywhere.** Dependencies are built once per ABI fingerprint (compiler, standard, flags, platform) into a shared store and linked into each project's `cpm_modules/`.
- **Static registries.** A registry is any directory or static HTTPS host. Archives are verified by SHA-256, and a matching prebuilt binary is used instead of building from source.
- **No dependencies.** CPM is plain C++23 with no third-party code, and it builds itself.

## Install

Prebuilt binaries are attached to each [GitHub Release](https://github.com/meirborowski/CPM/releases). Unpack the archive for your platform and put `cpm` (`cpm.exe` on Windows) on your `PATH`:

| Platform | Archive |
|---|---|
| Windows x64 | `cpm-<version>-x86_64-windows.zip` (statically linked, MinGW-w64) |
| Linux x64 | `cpm-<version>-x86_64-linux.tar.gz` (fully static, runs on any distribution) |
| macOS, Apple silicon | `cpm-<version>-aarch64-macos.tar.gz` (macOS 13.3 or newer) |

`SHA256SUMS` in the release lists the archives' checksums. CPM drives tools it finds on `PATH`: a C++ compiler (GCC 13+ or Clang 17+) and [Ninja](https://ninja-build.org) 1.10+ to build, plus `git`, `curl` and `tar` for git and registry dependencies.

## Quick start

```sh
cpm init -y hello     # scaffold an executable (or --lib, --shared-lib, --header-only)
cd hello
cpm run               # build and run it
```

A project needs only a manifest and a conventional layout:

```
hello/
  cpm.jsonc          { "name": "hello", "version": "0.1.0", "std": "c++23" }
  src/main.cpp       entrypoint (src/lib.cpp makes a library)
  src/greet.cpp      everything else in src/ is compiled and linked in
  tests/greet.test.cpp
```

```sh
cpm build             # debug build: build/debug/hello
cpm build --release   # release profile
cpm test              # builds and runs tests/*.test.cpp (exit code 0 = pass)
cpm run -- --flag     # build, then run the executable with arguments
cpm run lint          # run "scripts": { "lint": "..." } from cpm.jsonc
cpm i ../mylib        # add a local library (file:../mylib)
cpm i git+https://github.com/me/greet.git#v1.0.0   # add a library from git
cpm i fmt@^10         # add a package from the default registry
cpm remove mylib
```

## Registries

A registry is a base URL serving static files: `config.json` plus one `index/<name>.json` per package ([registry protocol](docs/spec/registry.md)). It can be a local directory (`file://`) or any static HTTPS host. CPM has no built-in registry, so configure one in `$CPM_HOME/config.jsonc` (`CPM_HOME` defaults to `~/.cpm`, or `%USERPROFILE%\.cpm` on Windows):

```jsonc
{
  "registries": {
    "default": "https://example.org/cpm-registry/",
    "local": "file:///C:/registries/local/"
  }
}
```

or set `CPM_REGISTRY` to a base URL. `cpm publish` is not implemented yet, so for now a registry's files are written by hand.

## Building from source

Requirements: GCC 13+ or Clang 17+, [Ninja](https://ninja-build.org) 1.10+, and `git`, all on `PATH`.

CPM is built with CPM. A fresh clone needs one bootstrap compile first, a single compiler call that takes a few minutes:

```sh
pwsh scripts/bootstrap.ps1                          # Windows; sh scripts/bootstrap.sh elsewhere
cpm_modules/.cache/bootstrap/cpm build --release    # CPM builds itself
build/release/cpm test
```

The result is `build/release/cpm` (`cpm.exe` on Windows). Put it on your `PATH`. CPM's tests use [doctest](https://github.com/doctest/doctest), a devDependency that CPM installs for itself from git, so the first build needs network access once. `scripts/selfhost.ps1` (or `scripts/selfhost.sh`) runs the full stage 0 → 1 → 2 self-hosting check.

## Documentation

The [specification](docs/spec/README.md) describes the manifest, dependency resolution, the lockfile, the store and ABI fingerprints, the registry protocol, the build and the CLI. [architecture.md](docs/spec/architecture.md) covers the source layout and the roadmap.

## Layout

| Path | Contents |
|---|---|
| `src/` | CPM's source: `cli/`, `init/`, `run/`, `install/`, `build/`, `toolchain/`, `lock/`, `manifest/`, `json/`, `semver/`, `core/` |
| `tests/` | Tests on doctest, one executable per `*.test.cpp` |
| `scripts/` | Bootstrap (stage 0) and the self-hosting check |
| `docs/spec/` | The specification |
| `schema/` | JSON Schemas for `cpm.jsonc`, `cpm-lock.json` and registry indexes |

## Releasing

[CI](.github/workflows/ci.yml) runs the self-hosting check on Windows, Linux (GCC and Clang) and macOS for every push and pull request. [Releases](.github/workflows/release.yml) are cut from tags:

1. Set the new version in `cpm.jsonc` and `src/version.hpp` (a test checks that they match) and commit.
2. Optional dry run: run the Release workflow by hand (Actions → Release → Run workflow). It builds, tests and packages every platform and uploads the archives as workflow artifacts, without publishing anything.
3. Tag and push, for example `git tag v0.2.0` and `git push origin v0.2.0`. The workflow checks the tag against the version, then publishes a GitHub Release with the archives and `SHA256SUMS`. Tags with a hyphen (`v0.2.0-rc.1`) become prereleases.

## License

[MIT](LICENSE)
