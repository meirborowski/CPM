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

CPM's tests use [cpm-test](https://github.com/cpm-org/cpm-test), a devDependency that CPM installs for itself from the default registry, so the first build needs network access once. `scripts/selfhost.ps1` (or `scripts/selfhost.sh`) runs the full stage 0 → 1 → 2 self-hosting check that CI runs.

## Layout

| Path | Contents |
|---|---|
| `src/` | CPM's source, one module per directory: `cli/`, `init/`, `run/`, `install/`, `publish/`, `build/`, `toolchain/`, `lock/`, `manifest/`, `json/`, `semver/`, `core/` |
| `tests/` | Tests on cpm-test, one executable per `*.test.cpp`; `support/check.hpp` holds the macros they use |
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

1. Write the release notes: a `## <version> — <date>` section at the top of [CHANGELOG.md](CHANGELOG.md), for people who use CPM. Start with one line on what the release is about, then what is new, what changed and what is fixed, each in terms of what users can now do. Leave out internal refactors. `sh scripts/release-notes.sh <version> --install` shows the notes as they will be published.
2. Set the new version in `cpm.jsonc` and `src/version.hpp` (a test checks that they match), and commit.
3. Optional dry run: run the Release workflow by hand (Actions → Release → Run workflow). It builds, tests and packages every platform and uploads the archives as workflow artifacts, without publishing anything.
4. Tag and push, for example `git tag v0.4.0` and `git push origin v0.4.0`. The workflow checks the tag against the version and that CHANGELOG.md has its notes. It then publishes a GitHub Release with the notes, how to install, the archives, the bare Windows executable and `SHA256SUMS`. Tags with a hyphen (`v0.4.0-rc.1`) become prereleases.
5. The [Installer](.github/workflows/installer.yml) workflow then runs the one-line installers on fresh Linux, macOS and Windows machines, once cpm-registry.com serves the new version.
