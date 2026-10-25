# Global store and `cpm_modules`

## 1. CPM home

`$CPM_HOME` is the `CPM_HOME` environment variable if set, otherwise `%USERPROFILE%\.cpm` on Windows and `$HOME/.cpm` elsewhere. Tests and CI SHOULD set `CPM_HOME` to an isolated directory.

```
$CPM_HOME/
  config.jsonc                     # user configuration (cli.md §5)
  toolchains.json                  # toolchain probe cache (build.md §1.3)
  store/
    .layout-version                # "1"
    <name>@<version>[+<suffix>]/   # store entries (§2)
  cache/
    downloads/sha256-<hex>.tar.gz  # downloaded archives, content-addressed
    git/<sha256(url)[:16]>.git/    # bare git mirrors
    registry/<sha256(base)[:16]>/index/<name>.json
  tmp/                             # staging and sandbox build directories (same volume as store/)
  locks/                           # lock files (§4)
  logs/                            # logs of dependency builds
  tools/                           # downloaded tools such as ninja (post-MVP)
```

The store and the caches can be shared by every project and every CPM process on the machine. Deleting `cache/` is always safe. Deleting `store/` is safe too: projects relink and rebuild on the next `cpm install`/`cpm build`. Losing only some of their files, for example to a cleaner of old temporary files, is handled as well: CPM notices damaged store entries (§2.3) and git mirrors (§3) and makes them again.

## 2. Store entries

### 2.1 Naming and contents

| Source | Entry directory |
|---|---|
| registry | `<name>@<version>` |
| registry, when an entry with that name already exists with a **different** integrity (same name and version from two registries) | `<name>@<version>+s<first 8 hex of integrity>` |
| git | `<name>@<version>+g<first 12 hex of commit>` |
| path | none. Path packages are used in place and built inside the consumer's build graph ([build.md §4.4](build.md#44-path-dependencies)). |

```
store/fmt@10.2.1/
  .integrity                 # "sha256:<hex>" (registry) or "git:<commit>" (git)
  .source.json               # the lock "resolution" object this entry came from
  .complete                  # written last: marks the source extraction as complete; lists files checked on reuse (§2.3)
  cpm.jsonc                  # the package manifest, if the archive has one
  .cpm-overlay.json          # the effective overlay, if any (canonical JSON)
  include/ …                 # as extracted (the include dir may be elsewhere if an overlay says so)
  src/ …
  lib/
    3f9a1c2b7d4e5a60/        # one directory per ABI (abi.md)
      libfmt.a
      meta.json
      .complete
```

- Everything outside `lib/` is the extracted package and is **immutable**. CPM never writes there after the entry is complete. It only replaces a damaged entry as a whole (§2.3).
- An entry without `.complete`, or an ABI directory without its own `.complete`, is garbage. It is deleted the next time CPM touches that entry, while holding its lock.
- On reuse, `.integrity` MUST equal the integrity CPM expects, otherwise the entry is treated as a store conflict (see the naming table).

### 2.2 `lib/<abi16>/meta.json`

```json
{
  "metaVersion": 1,
  "package": { "name": "fmt", "version": "10.2.1", "integrity": "sha256:493805ec…" },
  "abi": {
    "hash": "3f9a1c2b7d4e5a60c1…(64 hex)",
    "inputs": { "abi.scheme": "2", "arch": "x86_64", "compiler": "gcc", "…": "…" }
  },
  "artifacts": [
    { "kind": "static-lib", "file": "libfmt.a" }
  ],
  "linkName": "fmt",
  "publicDefines": [],
  "systemLibs": [],
  "builtBy": "cpm 0.1.0",
  "origin": "source"
}
```

| Key | Description |
|---|---|
| `abi.inputs` | The full fingerprint inputs ([abi.md](abi.md)). They are checked on every use: if the inputs differ under the same directory name (a hash prefix collision or a corrupt entry), the directory is treated as missing. |
| `artifacts[].kind` | `static-lib`, `shared-lib` (the runtime `.so`/`.dylib`/`.dll`) or `import-lib` (MinGW `.dll.a`, MSVC `.lib`). |
| `linkName` | The name passed to the linker (`-l<linkName>`). |
| `publicDefines`, `systemLibs` | Effective values after platform overrides. Dependents use them without re-reading the manifest. |
| `origin` | `source` (built locally) or `prebuilt` (downloaded from a registry). |

### 2.3 Damaged entries

Files under `$CPM_HOME` can disappear without CPM's involvement, for example when a cleaner deletes old files from a home kept in a temporary directory. Such a cleaner tends to take the package's files, which keep the old timestamps of their archive or commit, and leave CPM's own recently written files, `.complete` included. So `.complete` alone does not prove that an entry is usable. Every time CPM uses an entry (each install and build), it first runs a cheap check. The check reads one small file and tests that a few files exist. It never hashes or walks the whole package.

- **Source entry:** `.complete` lists a sample of the package's files, one path per line, relative to the entry and separated by `/`. Lines starting with `#` are comments. The sample is the first file of each top-level directory (looking at files before subdirectories, names in byte order), then the top-level files, at most 32 paths in all. Every listed file MUST exist. An empty `.complete`, written by CPM before this check existed, instead requires at least one file outside CPM's own files (`.complete`, `.integrity`, `.source.json`, `.cpm-overlay.json`) and `lib/`.
- **ABI directory:** `meta.json` MUST parse, and every `artifacts[].file` it lists MUST exist in the directory.

When the check fails, CPM prints a warning and, holding the item's lock (§4), deletes the entry or ABI directory. It then extracts or builds the item again, as if it had been missing. Deleting a source entry deletes its ABI directories too, so they are rebuilt (or their prebuilt binaries installed again) when needed. A prebuilt binary whose archive lacks an artifact its `meta.json` lists is not installed, because it would fail the check on every use. CPM builds the package from source instead.

The check catches the loss of a package's files as a whole or of whole directories. It does not catch every single missing file. A full integrity check belongs in `cpm store status` (§8).

## 3. Other home directories

- `cache/downloads/`: archives named by content hash. An archive found there with the right hash is not downloaded again. `--offline` fetches only from here.
- `cache/git/`: bare mirrors (`git clone --mirror`, then `git fetch` to update). A store entry is created from `git archive --format=tar <commit>` piped into the same extraction and validation path as source archives.
  - CPM runs git on a mirror with `--git-dir=<mirror>`, never `-C <mirror>`. If the mirror is damaged, git then fails, rather than finding a repository in a parent directory of `$CPM_HOME` and using that one.
  - A damaged mirror is cloned again, with a warning and under the mirror's lock. The new clone is staged in `tmp/` and replaces the old directory only once the clone succeeded. A mirror counts as damaged when its directory exists without `HEAD`, or when a fetch fails and a check shows the mirror is unusable: its `remote.origin.url` is not the package URL, or `git for-each-ref` cannot read the objects its refs point to. That check walks no history, so it stays fast for large repositories.
  - A fetch that fails while the mirror passes the check is a network or remote problem. CPM reports the error and keeps the mirror.
- `cache/registry/`: cached index documents ([registry.md §5](registry.md#5-client-behaviour)).
- `logs/`: `build-<name>@<version>-<abi16>.log`, the full output of each dependency build. CPM prints the log path and its last 40 lines when a build fails.

## 4. Atomicity and locking

- **Staging:** every store mutation is staged in `tmp/<random>/` and moved into place with a single `rename`. `tmp/` is on the same volume as `store/`, so the rename is atomic. The `.complete` marker is written into the staged directory before the rename.
- **Locks:** CPM holds an exclusive OS file lock (`LockFileEx` / `flock`) on `locks/<sha256(key)[:16]>.lock` while it fetches, extracts or builds a store item. `key` is the entry directory name for source extraction and `<entry>/lib/<abi16>` for builds. After taking the lock, CPM checks again whether the item became complete (and passes the check of §2.3) in the meantime. If CPM has to wait more than 1 second it prints `waiting for another cpm process (fmt@10.2.1)…`.
- Locks are advisory and released automatically if a process dies. Stale `tmp/` directories older than 24 hours are removed opportunistically.

## 5. `cpm_modules/`

```
<project>/
  cpm_modules/
    .cpm-modules.json              # link state (§5.1)
    fmt/
      include  → $CPM_HOME/store/fmt@10.2.1/include
      lib      → $CPM_HOME/store/fmt@10.2.1/lib/3f9a1c2b7d4e5a60
    json/
      include  → $CPM_HOME/store/json@3.11.3+g9cca280a0b1c/single_include   # overlay "include": "single_include"
    mylib/
      include  → <project>/../mylib/include                  # path package: no lib link
    .cache/                                                  # build intermediates (build.md §4.1)
      debug/   build.ninja  .ninja_log  .ninja_deps  archives.json  obj/…  lib/deps/…
      release/ …
      bootstrap/                                             # only in the CPM repository itself
  build/                                                     # build outputs (build.md §4.1)
    debug/   hello.exe  tests/…
    release/ …
  compile_commands.json
```

- `cpm_modules/<name>/` is a **real directory**. Only `include` and `lib` inside it are links. That leaves room for more entries later (`bin`, `share`) and avoids one link mixing headers with artifacts.
- `include` points to the package's effective include directory. `lib` points to the store ABI directory that matches the **active** ABI of the last build or install. When `cpm build` runs with a profile or toolchain whose ABI differs, CPM ensures the store has that ABI directory (building it if needed) and repoints `lib` before invoking Ninja.
- Header-only packages have no `lib`. Path packages have no `lib` because their objects live in `.cache/<profile>/obj/deps/<name>/`.
- `cpm_modules/` is disposable: deleting it loses nothing that `cpm install` cannot restore, usually without network access.

### 5.1 `.cpm-modules.json`

```json
{
  "cpmModulesVersion": 1,
  "lockHash": "sha256:…",
  "abi": "3f9a1c2b7d4e5a60",
  "linkMode": "junction",
  "packages": {
    "fmt": {
      "include": "C:/Users/me/.cpm/store/fmt@10.2.1/include",
      "lib": "C:/Users/me/.cpm/store/fmt@10.2.1/lib/3f9a1c2b7d4e5a60",
      "version": "10.2.1"
    },
    "mylib": { "include": "C:/work/mylib/include", "version": "0.3.0" }
  }
}
```

`cpm build` uses this file for its fast up-to-date check: the lock's SHA-256 equals `lockHash`, and the active ABI equals `abi`. If both match, no link or store work is done. Paths here are informative. The links themselves are authoritative and are verified when the check fails.

## 6. Link creation

CPM creates each link with the first method that works, and records it in `linkMode`:

| OS | Order |
|---|---|
| Linux, macOS | absolute symlink → copy |
| Windows | symlink (`CreateSymbolicLinkW` with `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, which works in Developer Mode) → **directory junction** (`FSCTL_SET_REPARSE_POINT`, needs no privileges, same machine only) → copy |

- The configuration key `linkMode` (`auto` | `symlink` | `junction` | `copy`) can force a method.
- When CPM falls back to `copy`, it warns once per project. Copies are refreshed whenever `.cpm-modules.json` is out of date.
- Hard links are not used for directories, which they cannot be applied to. File-level hard links are reserved for a future optimisation.

## 7. Safe deletion (normative)

Links into the store make naive recursive deletion **dangerous**. Some standard library implementations, including libstdc++ on MinGW, do not report junctions as symlinks. `std::filesystem::remove_all(cpm_modules)` could then descend into the store and delete it.

- CPM MUST delete trees only through `fsx::remove_tree`. That function never descends into a symlink or a reparse point of any kind (`FILE_ATTRIBUTE_REPARSE_POINT`). It removes such entries as links: `RemoveDirectoryW` for directory junctions and symlinks, `DeleteFileW` for file symlinks, `unlink` on POSIX.
- `fsx::remove_tree` refuses to delete `$CPM_HOME`, `$CPM_HOME/store`, the filesystem root, or the user's home directory.
- Removing a link never touches its target.

## 8. Cleaning up

The store keeps every package any project ever used, so it grows. `cpm cache clean` ([cli.md](cli.md#cpm-cache)) removes what no project uses any more. CPM knows which projects use the store because they register themselves.

### 8.1 `projects.json`

```json
{
  "projectsVersion": 1,
  "projects": {
    "C:/work/hello": { "lastUsed": "2026-10-20" }
  }
}
```

`cpm install` and `cpm build` record the project's root in `$CPM_HOME/projects.json`. That covers every command that installs or links dependencies. The path is absolute, normalized and `/`-separated. CPM writes the file only when the project is new or its `lastUsed` (UTC date) is older than today. It writes atomically, under the lock `locks/<sha256("projects.json")[:16]>.lock`. A project that has not run `cpm install` or `cpm build` since CPM began registering (0.4.1) is unknown.

### 8.2 What a project uses

For each package in a project's `cpm-lock.json` other than path packages:
- its store entry (§2.1);
- for a registry package, its archive in `cache/downloads/` (`sha256-<hex>.tar.gz`, the hex of its integrity);
- for a git package, its mirror in `cache/git/` (§3).

### 8.3 `cpm cache clean`

1. Registrations whose directory or `cpm.jsonc` no longer exists are removed.
2. Every store entry, downloaded archive and git mirror that no remaining project uses (§8.2) is deleted. Entries without `.complete` are garbage and go too.
   - A store entry is deleted under its lock and the locks of its `lib/<abi16>` directories (§4); a git mirror under its mirror lock. CPM only tries the locks: an item whose lock another process holds is kept, and reported.
   - A store entry or mirror is first renamed into `tmp/`, so it disappears in one step, and then deleted there through `fsx::remove_tree` (§7). One that cannot be renamed, because its files are open, is kept.
3. `tmp/` directories older than 24 hours are deleted.

`--all` deletes every store entry (not `store/` itself, which §7 protects), and everything under `cache/`, `tmp/` and `logs/`, under the same locks. It never touches `config.jsonc`, `credentials.json`, `projects.json`, `toolchains.json`, `tools/` or `bin/`.

Deleting a store entry or a cached download that a project still needs, including a project CPM doesn't know, only costs a rebuild. Its next `cpm install` or `cpm build` finds the entry missing and makes it again (§2.3), which may need network access for registry and git packages.
