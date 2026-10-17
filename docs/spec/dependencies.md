# Dependencies, versions and resolution

## 1. Dependency specifiers

`dependencies` and `devDependencies` map a **package name** to a **specifier**. The specifier is stored verbatim in the lockfile (`importers["."].…specifier`) and drives the lockfile freshness check ([lockfile.md §4](lockfile.md#4-freshness)).

### 1.1 String form

| Form | Kind | Example |
|---|---|---|
| `<range>` | registry (default registry) | `"^10.2.0"`, `"~1.4"`, `">=2 <3"`, `"*"`, `"1.2.3"` |
| `file:<path>` | path | `"file:../mylib"`, `"file:libs/core"` |
| `git+<url>[#<ref>]` | git | `"git+https://github.com/nlohmann/json.git#v3.11.3"` |
| `github:<owner>/<repo>[#<ref>]` | git (shorthand for `git+https://github.com/<owner>/<repo>.git`) | `"github:doctest/doctest#v2.4.11"` |

- `<path>` is relative to the directory of the manifest that declares it, or absolute. It is stored in the lock relative to the project root, with `/` separators.
- `<url>` may be `https://`, `ssh://`, `git@host:path` or `file://`. `<ref>` is a tag, a branch, or a full or abbreviated (≥ 7 hex digits) commit. Without `#<ref>` the remote's default branch is used. Branch refs are pinned to a commit in the lockfile.
- A string that is none of the above is parsed as a range. If that fails, it is an error.

### 1.2 Object form

```jsonc
"fmt":   { "version": "^10.2.0", "registry": "internal" },
"bar":   { "git": "https://example.com/bar.git", "tag": "v1.0.0",
           "manifest": { "type": "static-lib", "sources": ["lib/*.cpp"] } },
"local": { "path": "../local", "manifest": { "type": "header-only", "include": "." } }
```

| Key | Applies to | Meaning |
|---|---|---|
| `version` | registry (required), git | Registry: the range. Git: overrides the package version (§1.3). |
| `registry` | registry | Name of a registry from the configuration ([cli.md §5](cli.md#5-configuration)), or a registry base URL. Default: `default`. Registries are told apart by base URL, so a name and its URL denote the same source. A registry package's own dependencies come from the same registry. |
| `git` | git (required) | Repository URL. |
| `rev` / `tag` / `branch` | git | At most one of these. `rev` is a commit. |
| `path` | path (required) | Directory of the package. |
| `manifest` | git, path | Overlay ([manifest.md §8](manifest.md#8-manifest-overlays)). Registry packages get overlays from the index. |

Exactly one of `version` (without `git`), `git` or `path` determines the kind. For the lock's `specifier`, the object form is serialised canonically: keys sorted, compact JSON, for example `{"git":"https://example.com/bar.git","tag":"v1.0.0"}`. The `manifest` key is left out of that string.

### 1.3 Versions of git and path packages

Git and path packages always have exactly one candidate version, taken from the first of these that applies:

1. `version` in the dependency's object form or its overlay
2. `version` in the package's own `cpm.jsonc`
3. git only: the ref, if it is a tag that parses as a version once a leading `v` is stripped
4. git only: `0.0.0-git.<commit12>`; path only: `0.0.0`

Ranges that other packages declare on a git or path package are checked against that version. A mismatch is a normal resolution conflict.

### 1.4 CLI specifiers (`cpm install <spec>`)

| CLI argument | Written to the manifest as |
|---|---|
| `fmt` | `"fmt": "^<latest>"`, where latest = the highest non-prerelease, non-yanked version. With `--save-exact`: `"<latest>"`. |
| `fmt@^10`, `fmt@10.2.1` | the range exactly as typed: `"^10"`, `"10.2.1"` |
| `./x`, `../x`, `/abs/x`, `file:x` | `"<name>": "file:<path>"`, where `<name>` comes from the target's manifest |
| `git+…`, `github:…` | `"<name>": "<spec>"`, where `<name>` comes from the fetched manifest or overlay |
| `name@git+…` / `name@file:…` | the same, with an explicit name (required when the package has no manifest) |

## 2. Versions

Versions follow **SemVer 2.0.0**: `MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]`, no leading `v`, no leading zeros in numeric identifiers.
Precedence is as defined by SemVer §11. Build metadata is ignored for precedence and for range matching. Two versions that differ only in build metadata are the same version, so a registry MUST NOT publish both.

## 3. Ranges

CPM ranges follow npm's `node-semver` syntax and semantics, without its `loose` mode.

### 3.1 Grammar

```ebnf
range-set  ::= range ( ws* "||" ws* range )*
range      ::= hyphen | simple ( ws+ simple )* | ""
hyphen     ::= partial ws+ "-" ws+ partial
simple     ::= primitive | partial | tilde | caret
primitive  ::= ( "<" | ">" | ">=" | "<=" | "=" ) ws* partial
tilde      ::= "~" ws* partial
caret      ::= "^" ws* partial
partial    ::= xr ( "." xr ( "." xr qualifier? )? )?
xr         ::= "x" | "X" | "*" | nr
nr         ::= "0" | [1-9] [0-9]*
qualifier  ::= ( "-" pre )? ( "+" build )?
```

Whitespace inside a `range` means AND. `||` means OR. An empty range or `*` matches every non-prerelease version.

### 3.2 Desugaring

| Range | Equivalent |
|---|---|
| `1.2.3`, `=1.2.3` | `=1.2.3` |
| `1.2`, `1.2.x` | `>=1.2.0 <1.3.0-0` |
| `1`, `1.x` | `>=1.0.0 <2.0.0-0` |
| `~1.2.3` | `>=1.2.3 <1.3.0-0` |
| `~1.2` | `>=1.2.0 <1.3.0-0` |
| `~1` | `>=1.0.0 <2.0.0-0` |
| `^1.2.3` | `>=1.2.3 <2.0.0-0` |
| `^0.2.3` | `>=0.2.3 <0.3.0-0` |
| `^0.0.3` | `>=0.0.3 <0.0.4-0` |
| `^0.0` | `>=0.0.0 <0.1.0-0` |
| `^0` | `>=0.0.0 <1.0.0-0` |
| `1.2.3 - 2.3` | `>=1.2.3 <2.4.0-0` |
| `1.2 - 2.3.4` | `>=1.2.0 <=2.3.4` |
| `>1.2` | `>=1.3.0` |
| `<=1.2` | `<1.3.0-0` |

### 3.3 Prereleases

A prerelease version (`1.3.0-beta.2`) satisfies a comparator set only if:

- it satisfies every comparator in the set, **and**
- at least one comparator in the same set names a prerelease with the same `MAJOR.MINOR.PATCH`.

So `^1.3.0-beta.1` matches `1.3.0-beta.2` and `1.3.5`, but not `1.4.0-alpha`. Plain ranges such as `^1.2` never match prereleases.

### 3.4 Representation

`semver::Range` is stored as a **sorted union of disjoint intervals** `[lo, hi)` over version precedence, plus the set of `MAJOR.MINOR.PATCH` tuples for which prereleases are allowed. Intersection, union, complement and emptiness checks are exact. That is what a PubGrub solver needs (§4.4).

## 4. Resolution

### 4.1 Model

- **Flat graph:** every package name resolves to **exactly one** version across the whole graph. This is what protects against One Definition Rule violations by default.
- **Edges:** the root's `dependencies` and `devDependencies`, plus the `dependencies` of every resolved package. `devDependencies` of non-root packages are ignored.
- **Sources:** each name has a single source: a registry, a git URL and ref, or a directory. If two dependents ask for the same name from different sources, that is an error. The exception is when the root declares the name, in which case the root's declaration overrides every transitive one. This is how a git or path package can stand in for a registry package. Packages from git and registries may not depend on local paths.
- **Kinds:** in v0.1 a dependency must be a static or header-only library. Executables are rejected, and shared-library dependencies are *post-MVP* ([build.md §4.7](build.md#47-shared-libraries-at-run-time-post-mvp-for-shared-library-dependencies)).
- **Git refs:** each repository is mirrored once in `$CPM_HOME/cache/git` ([store.md §3](store.md#3-other-home-directories)). A tag or commit that is already in the mirror is used without contacting the remote. A branch or the default branch is fetched at most once per run. A commit pinned by the lockfile is reused as long as the specifier's URL and ref are unchanged, so branch dependencies stay put until the specifier changes or the package is reinstalled with a new ref.
- **Candidates:**
  - registry: the index versions that satisfy the intersection of every range on the name, excluding yanked versions unless the lock already pins that exact version
  - git/path: the single version from §1.3

### 4.2 Algorithm (MVP)

The MVP uses deterministic depth-first backtracking:

```
solve(assignments, pending):
    if pending is empty: return assignments
    name := the pending name with the fewest remaining candidates (ties: lexicographic)
    for version in order(candidates(name)):          # locked version first, then highest to lowest
        deps := dependencies(name, version)
        if every dep is compatible with assignments:
            r := solve(assignments + {name: version}, pending - name + new dep names)
            if r succeeds: return r
        record the conflict (name, version, the constraint it violated)
    fail with the recorded conflicts
```

- Candidate order: (1) the locked version, if it still satisfies; (2) the remaining non-prerelease versions, highest first; (3) prereleases, if any range allows them, highest first.
- Package metadata (versions, dependencies, type, std) comes from the **index entry** for registry packages, so no archive is downloaded during resolution. For git and path packages it comes from their manifest or overlay. Git repositories are fetched into the git cache to read it ([store.md §3](store.md#3-other-home-directories)).
- **Determinism:** for the same manifest, lockfile, configuration and index contents, the result MUST be the same.

### 4.3 Conflict reporting

When resolution fails, CPM prints the chain of constraints that cannot hold together:

```
error: no version of fmt satisfies all constraints
  fmt ^10.2.0   required by  hello (root)
  fmt ^9.1.0    required by  spdlog@1.12.0  ← required by hello (root) as ^1.12
  available: 9.1.0, 10.0.0, 10.1.1, 10.2.1
  hint: relax one of these constraints, or pick a spdlog version that accepts fmt 10
```

### 4.4 PubGrub (*post-MVP*)

The backtracking solver sits behind `install::Resolver`. A PubGrub implementation (incompatibilities, unit propagation, conflict-driven clause learning, derivation-tree explanations) will replace it without changing callers. It MUST return a solution whenever one exists. It may pick a different solution than the backtracking solver only when neither is pinned by the lock.

### 4.5 Interaction with the lockfile

| Command | Behaviour |
|---|---|
| `cpm install` (no args) | Locked versions are preferred and kept whenever they still satisfy the manifest. Only names whose root specifier changed, or that became unsatisfiable, are re-resolved. |
| `cpm install <pkg…>` | Like the above, but the named packages are unlocked and resolved to the newest version allowed by the new specifier. |
| `cpm install --frozen-lockfile` | The lock MUST be fresh ([lockfile.md §4](lockfile.md#4-freshness)). The solver does not run. Nothing is written. |
| `cpm remove <pkg…>` | The named entries are removed from the manifest. The remaining graph is re-resolved with every surviving lock entry pinned. Packages that are no longer reachable are dropped. |
| `cpm update [pkg…]` | Ignores lock preferences for the named packages, or for all of them, so they resolve to the newest versions their ranges allow. Git branches move to their current commit. The manifest is unchanged. |

## 5. ODR policy and symbol isolation

- By default a diamond that needs two incompatible versions of one package is a **resolution error** (§4.3). CPM never links two versions of the same package into one binary on its own.
- **Escape hatch (*post-MVP*):** `"isolate": true` on a dependency's object form lets that dependency keep a private copy of its own static-library dependencies:
  1. The isolated package and its private dependencies are resolved as a separate sub-graph. They are exempt from the flat rule for the names involved.
  2. All of them are compiled with `-fvisibility=hidden -fvisibility-inlines-hidden`. The isolated package's own exported API is marked visible through its public headers (`CPM_API`, emitted into a generated `cpm_export.hpp`).
  3. The objects are combined with `ld -r` into one relocatable object, and `objcopy --localize-hidden` is applied. The result is packaged as `lib<name>.a`, so private symbols cannot collide in the final link.
  4. Isolation is refused when the isolated package's public headers include headers of a private dependency. Types from a private dependency would then cross the boundary and violate the ODR again. CPM checks this by scanning the include graph of the public headers.
  5. On MSVC, isolation requires building the isolated package as a DLL (*later*).
