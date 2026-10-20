# Lockfile: `cpm-lock.json`

`cpm-lock.json` records the exact result of dependency resolution, so that every machine installs the same package versions from the same sources.
It is written by `cpm install` and `cpm remove`, is meant to be committed, and is **portable across machines and toolchains**: it holds no ABI hashes, no absolute paths and no timestamps.

## 1. Example

The lockfile for [examples/hello/cpm.jsonc](examples/hello/cpm.jsonc) (also in [examples/hello/cpm-lock.json](examples/hello/cpm-lock.json)):

```json
{
  "lockfileVersion": 1,
  "importers": {
    ".": {
      "dependencies": {
        "fmt": {
          "specifier": "^10.2.0",
          "version": "10.2.1"
        },
        "json": {
          "specifier": "{\"git\":\"https://github.com/nlohmann/json.git\",\"tag\":\"v3.11.3\"}",
          "version": "3.11.3"
        },
        "mylib": {
          "specifier": "file:../mylib",
          "version": "0.3.0"
        }
      },
      "devDependencies": {
        "doctest": {
          "specifier": "^2.4.11",
          "version": "2.4.11"
        }
      }
    }
  },
  "packages": {
    "doctest@2.4.11": {
      "dev": true,
      "resolution": {
        "integrity": "sha256:d19641fd9808927514ed24a2e0d0acc1c61158eee6aac60e2a68958e8a26587a",
        "registry": "https://registry.example.org/cpm/",
        "type": "registry",
        "url": "https://registry.example.org/cpm/packages/doctest/doctest-2.4.11.tar.gz"
      },
      "std": "c++11",
      "type": "header-only"
    },
    "fmt@10.2.1": {
      "resolution": {
        "integrity": "sha256:493805ecc9d155b64e2365962ae8e3927f8afea4a7c84e9e048db2c92b80253a",
        "registry": "https://registry.example.org/cpm/",
        "type": "registry",
        "url": "https://registry.example.org/cpm/packages/fmt/fmt-10.2.1.tar.gz"
      },
      "std": "c++11",
      "type": "static-lib"
    },
    "json@3.11.3": {
      "resolution": {
        "commit": "e4022eb3b738f5c88125b2c127212f359f14b12b",
        "ref": "v3.11.3",
        "type": "git",
        "url": "https://github.com/nlohmann/json.git"
      },
      "std": "c++11",
      "type": "header-only"
    },
    "mylib@0.3.0": {
      "dependencies": {
        "fmt": "10.2.1"
      },
      "resolution": {
        "path": "../mylib",
        "type": "path"
      },
      "std": "c++20",
      "type": "static-lib"
    }
  }
}
```

The hashes, commit and URLs are illustrative. Note the object-form specifier of `json`, serialised as canonical compact JSON.

## 2. Schema

Machine-readable: [`schema/cpm-lock.schema.json`](../../schema/cpm-lock.schema.json).

### 2.1 Top level

| Key | Type | Description |
|---|---|---|
| `lockfileVersion` | integer | Format version. Currently `1`. |
| `importers` | object | Map of importer path to importer. The MVP only has `"."` (the root). Workspaces (*post-MVP*) add relative paths such as `"libs/core"`. |
| `packages` | object | Map of `"<name>@<version>"` to package entry. It holds **every** resolved package, direct or transitive. |

### 2.2 Importer

| Key | Type | Description |
|---|---|---|
| `dependencies` | object | Name → `{ "specifier": string, "version": string }`. `specifier` is the manifest value verbatim (canonical JSON for object forms, [dependencies.md §1.2](dependencies.md#12-object-form)). |
| `devDependencies` | object | Same shape, for `devDependencies`. |
| `overrides` | object | The manifest's `overrides` ([dependencies.md §4.6](dependencies.md#46-overrides)), key to range, verbatim. Written only when the manifest has overrides. |

Empty `dependencies` and `devDependencies` are written as `{}`; neither key is ever omitted.

### 2.3 Package entry

| Key | Type | Present | Description |
|---|---|---|---|
| `resolution` | object | always | Where the package comes from (§2.4). |
| `type` | string | always | Resolved target type: `static-lib`, `shared-lib` or `header-only`. Executables cannot be dependencies. |
| `std` | string | always | The package's minimum C++ standard (after any overlay). |
| `dependencies` | object | if non-empty | Name → exact resolved version of this package's dependencies. |
| `dev` | bool | only when `true` | The package is reachable **only** through root `devDependencies`. |
| `overridden` | object | if non-empty | Name → this package's own range for a dependency whose range an override replaced. With the importer's `overrides`, it says exactly which requirements were changed, and from what. |

`type` and `std` can be derived from the pinned source. They are recorded anyway so that `cpm build` can check `std` compatibility and plan links without opening every package manifest, and so that reviewers can see in a diff when a package's type changes.

### 2.4 Resolution

| `type` | Fields | Notes |
|---|---|---|
| `registry` | `registry` (base URL), `url` (absolute archive URL), `integrity` | `integrity` = `"sha256:"` + 64 lowercase hex characters over the archive bytes. |
| `git` | `url`, `ref` (as requested; omitted if none), `commit` (40 hex) | The commit is the integrity: a git checkout is identified only by its commit. |
| `path` | `path` | Relative to the project root, `/` separators. Path packages are not immutable (see §4). |

## 3. Serialization (normative)

CPM MUST write the lockfile so that the same logical content always produces the same bytes:

1. UTF-8, no BOM, LF line endings, exactly one trailing newline.
2. Indentation: 2 spaces per level. Separator `": "` after keys, `","` at line end between members. No trailing commas, no comments.
3. Top-level keys in the fixed order `lockfileVersion`, `importers`, `packages`. **All other object keys are sorted** by byte-wise comparison of their UTF-8 encoding. This holds for `packages` keys, dependency maps and the fields inside entries.
4. Every non-empty object and array puts one member per line. Empty ones are written as `{}` / `[]` on one line.
5. Strings: escape `"`, `\` and control characters U+0000–U+001F (as `\n`, `\t`, `\r`, `\b`, `\f` or `\u00XX`, lowercase hex). Everything else, including `/` and non-ASCII characters, is written raw.
6. Integers only (`lockfileVersion`). No floating-point numbers occur.
7. Optional keys are written only when §2 says so (`dev` only when `true`, `dependencies` and `overridden` only when non-empty, `overrides` only when the manifest has some, `ref` only when present). A project without overrides therefore gets the same bytes as before overrides existed.

CPM writes the lockfile **only if its bytes change**, atomically (write to a temporary file, then rename). A lockfile that CPM reads and writes back unchanged MUST round-trip byte for byte.

## 4. Freshness

The lockfile is **fresh** for a manifest when all of the following hold:

1. `lockfileVersion` is supported.
2. `importers["."].dependencies` and `importers["."].devDependencies` have exactly the manifest's names, and each `specifier` equals the manifest's specifier string. `importers["."].overrides` equals the manifest's `overrides`.
3. Every `version` in the importers and every version in a package's `dependencies` refers to an existing `packages` key, and every `packages` entry is reachable from the importers. A lockfile that fails this check is corrupt, which is an error, not merely stale.
4. For every **path** package: its current manifest (plus overlay) still has the recorded `type` and `std` and the same dependency names. Each declared range, or the override that replaces it, is satisfied by the locked version, and the source kind (registry, git or path) of each dependency matches its locked resolution. Path packages can change on disk, so they are re-checked every time.

Registry and git packages are immutable, so their lock entries are never re-checked against their manifests.

- `cpm build`, `cpm run` and `cpm test` check freshness and run the install pipeline automatically when the lock is stale ([cli.md](cli.md#cpm-build)).
- `cpm install --frozen-lockfile` fails with exit code 1 when the lock is not fresh, and never writes it. CI should use this mode.

## 5. Versioning and migration

- A CPM version that finds a `lockfileVersion` **newer** than it supports MUST refuse to modify the lockfile. It reports the version and suggests upgrading CPM.
- A CPM version that reads an **older** `lockfileVersion` migrates it in memory and writes the new version on the next modifying command.
- *Post-MVP:* when the lockfile contains git merge-conflict markers, `cpm install` resolves it by re-running resolution with the union of both sides' pins as preferences.
