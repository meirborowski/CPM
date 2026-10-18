# Registry protocol (static index, API version 1)

A CPM registry is **a base URL that serves static files**. Installing needs no server-side logic, so a registry can be:

- a local directory: `file:///C:/registries/local/`, `file:///srv/cpm/`
- any static HTTPS host: GitHub Pages, `raw.githubusercontent.com/<org>/<repo>/<branch>/`, S3, nginx
- a git repository served through one of the above
- a **registry service** that serves the same files and also accepts publishes from signed-in users through an HTTP API (§7), the way npm's registry does

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
| `api` | string | Only for registry services (§7): the base URL of the write API, absolute or relative to the registry's base, e.g. `"api/v1/"`. Its presence tells clients that `cpm login` and `cpm publish` work against this registry. |

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
- **Authentication:** reads need none. Writes to a registry service (§7) carry `Authorization: Bearer <token>`, with the token from `cpm login` stored in `$CPM_HOME/credentials.json` (§7.2). CPM hands it to curl in a config file on standard input, never on the command line.

## 6. Publishing

`cpm publish [--registry <name|url>] [--out <dir>] [--dry-run]` ([cli.md](cli.md#cpm-publish)):

1. **Checks.** Refuses packages with `"private": true`, executables (registries hold `static-lib`, `shared-lib` and `header-only`), versions with build metadata, and `dependencies` that are git or path specs or name another registry. A published package may depend only on packages from the same registry. `devDependencies` are never published, so they may come from anywhere.
2. **Files.** Always `cpm.jsonc` and the top-level `LICENSE*`, `LICENCE*`, `COPYING*`, `NOTICE*` and `README*`, plus either the manifest's `files` globs or, by default, `CHANGELOG*`, `<include>/**`, `src/**`, the `sources` globs (or their defaults) and every platform's `sources`. Never `build/**`, `cpm-lock.json`, `cpm_modules/` or `.git/`. `!` globs remove matches, as in `sources`.
3. **Archive.** `<name>-<version>.tar.gz`, written by CPM itself so it is the same on every platform: POSIX tar (pax headers for long paths) with entries sorted by path, a single top-level directory `<name>-<version>/` (so `stripComponents: 1`), parent directories included, mtime 0, uid/gid 0, no user or group names, mode 0755 for directories and 0644 for files, inside gzip with mtime 0 and OS "unknown". The same files therefore always give the same integrity.
4. **Index.** The version entry has `source` (`packages/<name>/<name>-<version>.tar.gz`, the integrity, `stripComponents: 1`), `type`, `std` and `dependencies` from the manifest. No `manifest` overlay is needed, since the archive carries `cpm.jsonc`. It is added to `index/<name>.json`, which is created if needed and keeps versions oldest first. An existing document must be valid (§3), and a version that is already there is an error: published versions never change.
5. **Destination.** A registry service (one whose `config.json` has `api`) receives the archive through the publish endpoint (§7.3), which runs the same checks again on the server and writes the index itself. A `file://` registry is written in place, the archive first and then the index document, both atomically, under the lock file `.cpm-publish.lock` in the registry root. For any other registry, `--out <dir>` writes the same two files under `<dir>` (built on the registry's current index), to be committed or uploaded. Without `--out` that is an error. `--out` also works with a registry service, to inspect what would be uploaded.
6. `--dry-run` performs steps 1–4 against the registry's current index, lists the files, and writes nothing.
7. `--with-binary` (*post-MVP*) will also build the package for the local ABI and add a `binaries` entry.

## 7. Registry service API (v1)

A registry service is a server that answers the static reads of §1–§4 itself and adds an HTTP API for accounts and publishing, like npm's registry. Static clients cannot tell it apart from a static registry. Its `config.json` names the API base (`"api": "api/v1/"`), and every path below is relative to that base.

### 7.1 Conventions

- Requests and responses are JSON (`Content-Type: application/json`), except the archive upload of §7.3.
- Errors use the HTTP status code (400 bad request, 401 not signed in, 403 not allowed, 404 not found, 409 conflict, 413 too large, 429 slowed down) and the body `{ "error": "<code>", "message": "<sentence for people>" }`. `<code>` is a stable identifier such as `version_exists`. CPM prints the message.
- Authenticated requests carry `Authorization: Bearer <token>`. A token is an opaque string starting with `cpm_`. The server stores only its SHA-256.
- Reads (`config.json`, `index/<name>.json`, archives) need no token. The index is served with a short cache lifetime (at most a minute). Archives are immutable and cached forever.

### 7.2 Accounts and tokens

Accounts are GitHub accounts: the service never stores passwords. Signing in on the service's website uses GitHub's web OAuth flow. `cpm login` uses GitHub's device flow, relayed by the service so that the CLI needs no GitHub app of its own:

1. `POST login/device` → `{ "deviceCode", "userCode", "verificationUri", "interval", "expiresIn" }`. CPM prints the URL and the code, and opens the URL in a browser when it can.
2. CPM polls `POST login/device/token` with `{ "deviceCode" }` every `interval` seconds. It gets 428 `authorization_pending` while the user has not finished, 429 `slow_down` (add 5 seconds to the interval), 400 `expired_token` or `access_denied`, and finally 200 `{ "token", "login" }`.
3. CPM writes the token to `$CPM_HOME/credentials.json`, keyed by the registry's base URL. On POSIX it is created with mode 0600.

```json
{
  "credentialsVersion": 1,
  "registries": {
    "https://registry.example.org/": { "token": "cpm_…" }
  }
}
```

| Endpoint | Auth | Description |
|---|---|---|
| `GET whoami` | token | `{ "login", "name", "avatarUrl" }` |
| `GET tokens` | token | `{ "tokens": [{ "id", "name", "createdAt", "lastUsedAt" }] }`. Never the secret. |
| `POST tokens` | token | `{ "name" }` → `{ "id", "token" }`. The secret is shown only once. |
| `DELETE tokens/<id>` | token | Revokes a token. `DELETE tokens/current` revokes the one used for the request (`cpm logout`). |

### 7.3 Publishing

`PUT packages/<name>/<version>`, authenticated. The body is the `.tar.gz` of §6 (`Content-Type: application/gzip`, at most 20 MB), with two headers:

- `X-Cpm-Integrity: sha256:<hex>`: what the client computed.
- `X-Cpm-Package`: compact JSON `{ "type", "std", "dependencies" }`, the index entry fields the client derived from the manifest (§6 step 4).

The server re-checks everything rather than trusting the client:

- the name and version are valid (no build metadata), and the integrity matches the body
- the archive is gzip and POSIX tar, holding only regular files and directories, all inside `<name>-<version>/`, with no absolute paths and no `..`
- `<name>-<version>/cpm.jsonc` exists, its `name` and `version` match the URL, and its `dependencies` are registry ranges equal to the declared ones
- the declared `std` equals the manifest's (default `c++20`), the declared `type` is a library type, and it equals the manifest's `type` when the manifest has one
- the package is new, in which case the publisher becomes its first owner, or the publisher is an owner
- the version is not published yet (409 `version_exists`). Published versions never change, and yanked ones are no exception.

On success: 201 `{ "name", "version", "integrity" }`. The new version appears in `index/<name>.json` straight away, as `source.url` `packages/<name>/<name>-<version>.tar.gz` with `stripComponents: 1`.

A service may limit storage. It answers 507 `registry_full` when it accepts no more uploads, and 429 `upload_quota` when an account has uploaded its allowance for now. Clients show the message and do not retry.

### 7.4 Yanking and owners

| Endpoint | Auth | Description |
|---|---|---|
| `POST packages/<name>/<version>/yank` | owner | Marks the version `yanked` in the index: new resolutions skip it, existing lockfiles keep working (§3). |
| `POST packages/<name>/<version>/unyank` | owner | Reverses it. |
| `GET packages/<name>/owners` | — | `{ "owners": ["<login>", …] }` |
| `PUT packages/<name>/owners/<login>` | owner | Adds an owner. The account must have signed in to the service once. |
| `DELETE packages/<name>/owners/<login>` | owner | Removes an owner. The last owner cannot be removed. |

### 7.5 Browsing

For the service's website and a future `cpm search`:

| Endpoint | Description |
|---|---|
| `GET search?q=<text>&page=<n>` | `{ "packages": [{ "name", "description", "latest", "type", "license" }], "total", "page" }`, matching names and descriptions, 20 per page |
| `GET packages/<name>` | `{ "name", "description", "license", "repository", "homepage", "readme", "latest", "owners", "versions": [{ "version", "publishedAt", "yanked", "type", "std", "dependencies", "size" }] }` |

## 8. Installing CPM

CPM's built-in registry service also installs CPM itself, rustup-style. Other registries need not offer any of this.

### 8.1 Install scripts

```
irm https://cpm-registry.com/install.ps1 | iex                 # Windows (PowerShell 5.1 or 7)
curl -fsSL https://cpm-registry.com/install.sh | sh             # Linux and macOS
```

Both ask the service for the latest release (§8.3) and download the binary for the machine: the bare `.exe` on Windows, the `.tar.gz` elsewhere. They check its SHA-256, and install it as `$CPM_HOME/bin/cpm` (default `~/.cpm/bin`), replacing an older one. They then put that directory on the user's `PATH`: the user `Path` variable on Windows; a line sourcing `$CPM_HOME/env` added to `~/.profile`, `~/.bashrc` and `~/.zshrc` (those that exist) elsewhere. Finally they run `cpm setup` ([cli.md](cli.md#cpm-setup)). Every step is idempotent, so running a script again updates CPM. Nothing needs administrator rights, except the Linux packages `cpm setup` installs.

Options go in environment variables:
- `CPM_HOME` says where to install. The script makes the choice last: it adds the variable to the user's environment on Windows, and to `$CPM_HOME/env` elsewhere.
- `CPM_NO_SETUP=1` skips `cpm setup`.
- `CPM_NO_MODIFY_PATH=1` leaves `PATH`, `CPM_HOME` and shell profiles alone.

On Linux and macOS the profile line goes into `~/.profile`, created if missing, and into `~/.bash_profile`, `~/.bashrc` and `~/.zshrc` where they exist. `~/.zshrc` is also created when the login shell is zsh.

### 8.2 `toolchains.json`

`GET toolchains.json` at the registry's base lists the tools `cpm setup` may download, per platform:

```json
{
  "toolchainsVersion": 1,
  "platforms": {
    "x86_64-windows": {
      "compiler": {
        "name": "WinLibs GCC 16.2.0 (MinGW-w64 14.0.0, UCRT)",
        "version": "16.2.0",
        "url": "https://github.com/brechtsanders/winlibs_mingw/releases/download/…/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r2.zip",
        "sha256": "d5dbafc4…",
        "size": 273613326,
        "cxx": "mingw64/bin/g++.exe"
      },
      "ninja": { "version": "1.13.2", "url": "…/ninja-win.zip", "sha256": "07fc8261…", "size": 291570, "exe": "ninja.exe" }
    },
    "x86_64-linux": { "ninja": { … } }
  }
}
```

- Platform keys are `<arch>-<os>`: `x86_64` or `aarch64`, and `windows`, `linux` or `macos`.
- Each tool is a `.zip` archive. `cxx` and `exe` are paths inside it, `/`-separated.
- `sha256` is 64 lowercase hex characters over the archive, and `size` is its length in bytes, shown before downloading.
- A tool goes into `$CPM_HOME/tools/<tool>-<version>/` (`compiler` is stored as `winlibs`).

### 8.3 The latest release

| Endpoint | Description |
|---|---|
| `GET api/v1/cpm/latest` | `{ "version", "assets": { "<target>": { "url", "sha256" } } }`, where target is `x86_64-windows`, `x86_64-linux` or `aarch64-macos` |
| `GET api/v1/cpm/latest/<target>` | The same for one target as one line of text, `<version> <url> <sha256>`, for `sh` |

The service reads CPM's latest GitHub release and caches the answer for 10 minutes.
