# Contributing to CPM

Thanks for helping. This page covers building CPM from source, running the tests, how the code is laid out, and how releases are made. The design is written down in the [specification](docs/spec/README.md); [architecture.md](docs/spec/architecture.md) describes the modules and the roadmap.

## Building from source

Requirements: GCC 13+ or Clang 17+, [Ninja](https://ninja-build.org) 1.10+, and `git`, all on `PATH`.

CPM is built with CPM. A fresh clone needs one bootstrap compile first, a single compiler call that takes a few minutes:

```sh
pwsh scripts/bootstrap.ps1                          # Windows; sh scripts/bootstrap.sh elsewhere
cpm_modules/.cache/bootstrap/cpm build --release    # CPM builds itself
build/release/cpm test
```

The result is `build/release/cpm` (`cpm.exe` on Windows). From then on, develop with it: `cpm build`, `cpm test`, `cpm run`.

CPM's tests use [doctest](https://github.com/doctest/doctest), a devDependency that CPM installs for itself, so the first build needs network access once. `scripts/selfhost.ps1` (or `scripts/selfhost.sh`) runs the full stage 0 → 1 → 2 self-hosting check that CI runs.

## Layout

| Path | Contents |
|---|---|
| `src/` | CPM's source, one module per directory: `cli/`, `init/`, `run/`, `install/`, `publish/`, `build/`, `toolchain/`, `lock/`, `manifest/`, `json/`, `semver/`, `core/` |
| `tests/` | Tests on doctest, one executable per `*.test.cpp` |
| `scripts/` | Bootstrap (stage 0) and the self-hosting check |
| `docs/spec/` | The specification |
| `schema/` | JSON Schemas for `cpm.jsonc`, `cpm-lock.json` and registry indexes |

## Guidelines

- C++23, limited to what GCC 13, Clang 17 and MSVC 19.4x all support ([architecture.md §6](docs/spec/architecture.md#6-implementation-constraints)).
- No third-party code in CPM itself.
- A behaviour change updates the spec in the same change.
- Every change keeps `cpm test` and the self-hosting check green on all CI platforms.

## Releasing

[CI](.github/workflows/ci.yml) runs the self-hosting check on Windows, Linux (GCC and Clang) and macOS for every push and pull request. [Releases](.github/workflows/release.yml) are cut from tags:

1. Set the new version in `cpm.jsonc` and `src/version.hpp` (a test checks that they match), update the download links in `README.md`, and commit.
2. Optional dry run: run the Release workflow by hand (Actions → Release → Run workflow). It builds, tests and packages every platform and uploads the archives as workflow artifacts, without publishing anything.
3. Tag and push, for example `git tag v0.3.0` and `git push origin v0.3.0`. The workflow checks the tag against the version, then publishes a GitHub Release with the archives, the bare Windows executable and `SHA256SUMS`. Tags with a hyphen (`v0.3.0-rc.1`) become prereleases.
