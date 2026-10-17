# Registry protocol (static index, API version 1)

A CPM registry is **a base URL that serves static files**. No server-side logic is needed, so a registry can be:

- a local directory: `file:///C:/registries/local/`, `file:///srv/cpm/`
- any static HTTPS host: GitHub Pages, `raw.githubusercontent.com/<org>/<repo>/<branch>/`, S3, nginx
- a git repository served through one of the above

All registry files are strict JSON (no comments), UTF-8. CPM normalises a base URL to end with `/`, so relative URLs resolve inside it (RFC 3986).

## 1. Layout

```
<base>/
  config.json                                   # required
  index/<name>.json                             # one document per package (§3)
  packages/<name>/<name>-<version>.tar.gz       # source archives (recommended location)
  binaries/<name>/<version>/<abi16>.tar.gz      # prebuilt binaries (recommended location)
```

Only `config.json` and `index/<name>.json` have fixed paths. Archive URLs are whatever the index says. Relative URLs are resolved against `<base>/` (RFC 3986), so the `packages/` and `binaries/` locations are only a convention.

## 2. `config.json`

```json
{
  "apiVersion": 1,
  "name": "example-registry"
}
```

| Key | Type | Description |
|---|---|---|
| `apiVersion` | integer | **Required.** `1`. A client MUST refuse a registry whose `apiVersion` it does not support. |
| `name` | string | Human-readable name, used in messages. |

## 3. Index documents

`index/<name>.json` lists every published version of one package:

```json
{
  "name": "fmt",
  "versions": {
    "10.2.1": {
      "source": {
        "url": "packages/fmt/fmt-10.2.1.tar.gz",
        "integrity": "sha256:493805ecc9d155b64e2365962ae8e3927f8afea4a7c84e9e048db2c92b80253a",
        "stripComponents": 1
      },
      "type": "static-lib",
      "std": "c++11",
      "dependencies": {},
      "manifest": {
        "type": "static-lib",
        "std": "c++11",
        "sources": ["src/format.cc", "src/os.cc"],
        "include": "include"
      },
      "binaries": {
        "3f9a1c2b7d4e5a60": {
          "url": "binaries/fmt/10.2.1/3f9a1c2b7d4e5a60.tar.gz",
          "integrity": "sha256:09361a5e3989a3ca1c3e76a6668aba436b053e4334ed3fe8efd1e5493a267283"
        }
      }
    }
  }
}
```

| Key | Type | Description |
|---|---|---|
| `name` | string | Must equal the file name and the package name. |
| `versions` | object | Map of version to entry. Versions are SemVer 2.0.0 without build metadata. |

**Version entry:**

| Key | Type | Required | Description |
|---|---|---|---|
| `source.url` | string | yes | Source archive URL (absolute, or relative to the base). |
| `source.integrity` | string | yes | `sha256:<64 lowercase hex>` over the archive bytes. |
| `source.stripComponents` | integer | no (0) | Number of leading path components to strip on extraction, as with `tar --strip-components`. |
| `type` | string | yes | `static-lib`, `shared-lib` or `header-only`. |
| `std` | string | yes | Minimum C++ standard. |
| `dependencies` | object | yes | Name → range. **Authoritative for resolution**, so it MUST match the package manifest or overlay. Published packages may depend only on registry packages: git and path dependencies are rejected at publish time. |
| `manifest` | object | no | Overlay ([manifest.md §8](manifest.md#8-manifest-overlays)). Required if the archive has no `cpm.jsonc`. |
| `binaries` | object | no | Map of ABI directory name (16 hex, [abi.md](abi.md)) to `{ url, integrity }`. |
| `yanked` | bool | no (false) | Yanked versions are skipped by resolution unless already pinned by a lockfile. |

**Immutability:** a published `source` (URL contents and integrity) MUST never change. `binaries` MAY be added later, and `yanked` MAY be toggled. A client that sees a pinned version's integrity differ from its lockfile MUST fail with an integrity error.

## 4. Archives

### 4.1 Source archives

- gzip-compressed POSIX tar (ustar or pax), file extension `.tar.gz`.
- After applying `stripComponents`, the archive root is the package root: `cpm.jsonc` (unless an overlay is given), `include/`, `src/`, license files and so on.
- Entries MUST be regular files or directories. Symlinks, hard links, device files and FIFOs are rejected. Absolute paths and any `..` component are rejected. CPM extracts into an empty staging directory and validates every extracted path before moving it into the store ([store.md §4](store.md#4-atomicity-and-locking)).

### 4.2 Binary archives

- Same format. The archive root holds exactly the contents of a store `lib/<abi16>/` directory: the built artifacts plus `meta.json` ([store.md §2.2](store.md#22-libabi16metajson)).
- After download and extraction, CPM MUST check that `meta.json` → `abi.hash` starts with the requested `<abi16>` and that its `abi.inputs` equal the locally computed inputs. On a mismatch it discards the binary, builds from source, and warns.
- Headers are **not** part of binary archives. They always come from the source archive, so a binary hit still downloads the source archive. Source archives are usually small next to the build time they save.

## 5. Client behaviour

- **Fetching the index:** `GET <base>/index/<name>.json`. A 404 (or a missing file for `file://`) means "no such package". Responses are cached in `$CPM_HOME/cache/registry/<sha256(base)[:16]>/index/<name>.json` with their fetch time.
  - Within one CPM process, each index document is fetched at most once.
  - Cached documents younger than 5 minutes are reused without a request.
  - `--offline` uses only cached documents and fails if one is missing.
  - `--prefer-offline` uses cached documents of any age and fetches only missing ones.
- **Downloads:** `https://` goes through `curl -sS -L --retry 2 --proto =https -o <tmp> -w %{http_code} <url>` (`--proto =https,http` when the registry allows http); a 404 or 410 means "not found", other non-2xx statuses are errors. `file://` is copied natively. `http://` is refused unless the registry is configured with `"allowHttp": true`. If fetching an index fails but a cached copy exists, CPM warns and uses the cached copy.
- **Verification:** the archive's SHA-256 is checked before it is extracted or cached. On a mismatch the file is deleted and CPM fails with `integrity check failed for fmt@10.2.1 (expected sha256:…, got sha256:…)`.
- **Authentication** (*post-MVP*): a per-registry `token` in the configuration, sent as `Authorization: Bearer <token>` (via a curl config file passed on stdin, never on the command line).

## 6. Publishing

`cpm publish [--registry <name|url>] [--out <dir>] [--dry-run]` ([cli.md](cli.md#cpm-publish)):

1. **Checks.** Refuses packages with `"private": true`, executables (registries hold `static-lib`, `shared-lib` and `header-only`), versions with build metadata, and `dependencies` that are git or path specs or name another registry. A published package may depend only on packages from the same registry. `devDependencies` are never published, so they may come from anywhere.
2. **Files.** Always `cpm.jsonc` and the top-level `LICENSE*`, `LICENCE*`, `COPYING*`, `NOTICE*` and `README*`, plus either the manifest's `files` globs or, by default, `CHANGELOG*`, `<include>/**`, `src/**`, the `sources` globs (or their defaults) and every platform's `sources`. Never `build/**`, `cpm-lock.json`, `cpm_modules/` or `.git/`. `!` globs remove matches, as in `sources`.
3. **Archive.** `<name>-<version>.tar.gz`, written by CPM itself so it is the same on every platform: POSIX tar (pax headers for long paths) with entries sorted by path, a single top-level directory `<name>-<version>/` (so `stripComponents: 1`), parent directories included, mtime 0, uid/gid 0, no user or group names, mode 0755 for directories and 0644 for files, inside gzip with mtime 0 and OS "unknown". The same files therefore always give the same integrity.
4. **Index.** The version entry has `source` (`packages/<name>/<name>-<version>.tar.gz`, the integrity, `stripComponents: 1`), `type`, `std` and `dependencies` from the manifest. No `manifest` overlay is needed, since the archive carries `cpm.jsonc`. It is added to `index/<name>.json`, which is created if needed and keeps versions oldest first. An existing document must be valid (§3), and a version that is already there is an error: published versions never change.
5. **Destination.** A `file://` registry is written in place, the archive first and then the index document, both atomically, under the lock file `.cpm-publish.lock` in the registry root. For any other registry, `--out <dir>` writes the same two files under `<dir>` (built on the registry's current index), to be committed or uploaded. Without `--out` that is an error.
6. `--dry-run` performs steps 1–4 against the registry's current index, lists the files, and writes nothing.
7. `--with-binary` (*post-MVP*) will also build the package for the local ABI and add a `binaries` entry.
