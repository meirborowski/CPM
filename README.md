# CPM — C++ Package Manager

CPM brings the npm/pnpm workflow to C++: one declarative manifest (`cpm.jsonc`), a deterministic lockfile, a project-local `cpm_modules/`, a global ABI-keyed binary cache, and direct Ninja generation. No CMake.

> **Status: early and experimental (v0.1).** `cpm init`, `install`, `remove`, `build`, `run` and `test` work with GCC and Clang, for registry, git and path dependencies. So far CPM has only been tested on Windows with MinGW-w64 GCC 13. The Linux and macOS code paths exist but have not been run yet. MSVC is not supported yet, and there is no public package registry yet (see [Registries](#registries)).

## Why CPM

- **One manifest, no CMake.** A `cpm.jsonc` and a conventional layout are the whole build description. CPM writes Ninja files directly.
- **Reproducible installs.** `cpm-lock.json` pins every version, git commit and archive hash.
- **Build once, reuse everywhere.** Dependencies are built once per ABI fingerprint (compiler, standard, flags, platform) into a shared store and linked into each project's `cpm_modules/`.
- **Static registries.** A registry is any directory or static HTTPS host. Archives are verified by SHA-256, and a matching prebuilt binary is used instead of building from source.
- **No dependencies.** CPM is plain C++23 with no third-party code, and it builds itself.

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

## License

[MIT](LICENSE)
