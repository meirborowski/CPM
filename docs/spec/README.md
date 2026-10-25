# CPM Specification

**Version:** 0.1 (draft) · **Date:** 2026-10-15 · **Status:** MVP design. Sections marked *post-MVP* describe intended behaviour that is not yet scheduled for implementation.

CPM (C++ Package Manager) is a package manager and build system for C++ that brings the npm/pnpm workflow to C++:
a declarative manifest (`cpm.jsonc`), a deterministic lockfile (`cpm-lock.json`), a project-local `cpm_modules/` directory,
a global content-verified store shared by all projects, ABI-keyed binary caching, and direct Ninja generation (no CMake).

```
cpm init -y            # scaffold a project
cpm i fmt@^10          # resolve, fetch (prebuilt or build from source), link into ./cpm_modules
cpm build --release    # generate build.ninja and compile
cpm run                # build and run the executable
```

## Documents

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Goals, the four pillars, module layering, internal data structures, implementation constraints, bootstrap and self-hosting, milestones |
| [cli.md](cli.md) | Commands, options, exit codes, output, configuration file, environment variables |
| [manifest.md](manifest.md) | `cpm.jsonc`: JSONC dialect, every field, convention-based target inference, profiles, platform overrides, comment-preserving edits |
| [dependencies.md](dependencies.md) | Dependency specifiers, SemVer and range grammar, flat resolution algorithm, ODR policy |
| [lockfile.md](lockfile.md) | `cpm-lock.json`: schema, deterministic serialization, freshness rules |
| [registry.md](registry.md) | Static registry protocol: index documents, source and binary archives, integrity |
| [store.md](store.md) | Global store (`~/.cpm`), `cpm_modules/` layout, linking, atomicity, locking |
| [abi.md](abi.md) | ABI fingerprint: inputs, probing, canonical encoding, hashing |
| [build.md](build.md) | Toolchain discovery, drivers, profile→flag mapping, build plan, Ninja generation, dependency builds, tests |

Machine-readable schemas (JSON Schema draft-07) live in [`/schema`](../../schema):
`cpm.schema.json` (manifest), `cpm-lock.schema.json` (lockfile), `registry-index.schema.json` (registry index documents).
The registry schema reuses manifest definitions through `urn:cpm:schema:manifest:1#/…`, so validators must have `cpm.schema.json` loaded as well.
Worked examples are in [`examples/`](examples).

## Conventions

- The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are to be interpreted as described in RFC 2119.
- `$CPM_HOME` denotes the CPM home directory: the `CPM_HOME` environment variable if set, otherwise `~/.cpm`
  (`%USERPROFILE%\.cpm` on Windows). See [store.md §1](store.md#1-cpm-home).
- Paths in documents, manifests and lockfiles use `/` as separator on every OS. CPM converts to native separators only when calling OS APIs.
- "Project root" is the directory containing the `cpm.jsonc` of the package being operated on.
- Example output is non-normative unless stated otherwise.

## Glossary

| Term | Meaning |
|---|---|
| **package** | A directory with a `cpm.jsonc` (or a manifest overlay), identified by `name@version`. |
| **project / root package** | The package CPM is invoked on. Only the root's `profiles` and `devDependencies` are honoured. |
| **importer** | A package whose dependencies are recorded in the lockfile (`.` = the root; more with workspaces, *post-MVP*). |
| **specifier** | The right-hand side of a dependency entry, exactly as written (`^10.2.0`, `file:../x`, `git+https://…#v1`). |
| **resolution** | The concrete source a specifier resolved to (registry archive + integrity, git commit, or local path). |
| **target** | A build output: executable, static library, shared library, or test executable. |
| **profile** | A named set of abstract build settings (`debug`, `release`, custom). |
| **ABI fingerprint / ABI hash** | Hash over the toolchain and settings that determine binary compatibility; keys compiled artifacts in the store. |
| **store entry** | `$CPM_HOME/store/<name>@<version>/`: an extracted package plus compiled artifacts under `lib/<abi16>/`. |
| **overlay** | A partial manifest supplied from outside a package (registry index or dependency spec) for packages that ship no `cpm.jsonc`. |

## Format versions

| Artifact | Version marker | Current |
|---|---|---|
| `cpm.jsonc` | none (additive evolution; unknown keys warn) | — |
| `cpm-lock.json` | `lockfileVersion` | `1` |
| Registry | `config.json` → `apiVersion` | `1` |
| Store layout | `$CPM_HOME/store/.layout-version` | `1` |
| ABI fingerprint scheme | `abi.scheme` input | `2` |
| `cpm_modules/.cpm-modules.json` | `cpmModulesVersion` | `1` |
