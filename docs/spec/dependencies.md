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

`semver::Range` is stored as a **sorted union of disjoint intervals** `[lo, hi)` over version precedence, plus the set of `MAJOR.MINOR.PATCH` tuples for which prereleases are allowed. Intersection and membership are exact. The resolver doesn't do algebra on ranges: it turns each range into the set of existing versions it contains (§4.2).

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

### 4.2 Algorithm

CPM resolves with PubGrub, the conflict-driven algorithm of Dart's pub (also used by uv and Poetry). It learns from each conflict, so it doesn't retry combinations that are known to fail, and it can say why no solution exists (§4.3).

- **Version sets.** Each package name has a fixed, finite list of versions to choose from: the registry index's versions (yanked ones left out, unless the lockfile pins that version), or the single version of a git or path package. Every set of versions the solver works with is a subset of that list. A dependency's range becomes the versions in the list that it contains (§3).
- **Terms and incompatibilities.** A term says that a package is selected with a version in a set, such as `fmt ^10.2.0`, or that it isn't (`not fmt ^9.1.0`). An incompatibility is a set of terms that can't all be true at once. For example, `{spdlog >=1.12.0, not fmt ^9.1.0}` says that those spdlog versions can't be used without fmt ^9.1.0. Resolution starts from the incompatibility "the project must be selected".
- **Dependencies.** When the solver considers a version, it adds one incompatibility per dependency. For a registry package, the incompatibility covers the run of neighbouring versions, in version order, whose index entries declare the same requirement. One incompatibility can then cover `spdlog >=1.12.0` instead of one per version, which keeps explanations short.
- **The loop.**
  1. *Unit propagation* derives what the incompatibilities force: when every term but one is true, the last one must be false.
  2. When an incompatibility becomes fully true, *conflict resolution* combines it with the incompatibilities that caused its terms, until it finds the root cause. It records that root cause as a new incompatibility and jumps back to the last decision it doesn't depend on.
  3. When nothing more is forced, CPM *decides* a package: among the required packages not yet decided, the one with the fewest allowed versions (ties by name), at its best allowed candidate. When no version is allowed, that fact becomes an incompatibility.
  4. It stops when every required package has a version, or when it learns that the project itself can't be satisfied.
- **Candidate order:** (1) the locked version, if it is allowed; (2) the other non-prerelease versions, highest first; (3) prereleases, highest first, for ranges that allow them (§3.3).
- **Sources first.** A name's source is fixed before its versions are chosen (§4.1): by the project when the project declares the name, otherwise by the first dependent CPM reads. Two dependents asking for one name from different sources is an error that names both. It is not explained as a conflict, since no choice of versions changes it.
- **Missing packages.** A project dependency on a name that the registry doesn't have is an error. A package version that depends on a missing name can't be used, and resolution goes on without it.
- Package metadata (versions, dependencies, type, std) comes from the **index entry** for registry packages, so no archive is downloaded during resolution. For git and path packages it comes from their manifest or overlay. Git repositories are fetched into the git cache to read it ([store.md §3](store.md#3-other-home-directories)).
- **Determinism:** for the same manifest, lockfile, configuration and index contents, the result MUST be the same.

### 4.3 Conflict reporting

When no solution exists, CPM explains why, as a chain of reasons that ends at the project:

```
error: the project's dependencies can't be resolved
  Because spdlog >=1.12.0 depends on fmt ^9.1.0 and the project depends on fmt ^10.2.0, spdlog >=1.12.0 can't be used.
  So, because the project depends on spdlog ^1.12.0, no versions satisfy every requirement.
  hint: change the range of fmt or spdlog in cpm.jsonc; cpm info <package> shows what each version depends on
```

- Each line puts two facts together and says what follows from them. The facts are what a package version depends on, what the project depends on, and what earlier lines concluded. A requirement that nothing can meet says why: `a 1.0.0 depends on fmt ^99.0.0, which matches no version (available: 12.2.0, 12.1.0, …)`, `…, which doesn't match mylib 2.0.0 (path ../mylib)` or `…, which <registry> doesn't have`. A chain of dependencies reads in order, from the dependent to what it needs.
- The first line of a part starts with "Because", the lines after it with "And because", and the last line of the explanation with "So, because". When nothing needs combining, the explanation is a single sentence, such as `The project depends on fmt ~12.1.0, which matches no version (available: 12.2.0).`
- A conclusion that a later line refers back to gets a number, such as `(1)`, at the start of its line, and the later line cites it by that number. An explanation that has to follow two separate branches shows them as parts separated by a blank line.
- A version set reads as the range it came from when it came from one (`fmt ^9.1.0`). Otherwise it is written with versions that exist: `spdlog 1.17.0`, `spdlog >=1.12.0`, `spdlog <=1.15.0`, `spdlog >=1.12.0 <=1.15.0`, several of those joined by `||`, or `every version of spdlog`. Git and path packages show their source, such as `mylib 0.1.0 (path ../mylib)`.
- The hint names the packages whose range, or git or path source, in the project's `cpm.jsonc` is part of the explanation, since those are what the project can change. When other packages' dependencies are part of it too, the hint points to `cpm info`, which lists what each version depends on.
- In a project that installed before, `cpm why <package>` ([cli.md](cli.md#cpm-why)) shows which dependencies hold the ranges involved.

### 4.4 Guarantees

- Resolution finds a solution whenever one exists. It fails only when none does, and then the explanation of §4.3 is a proof of that.
- Requirements are never changed or ignored behind the project's back. Overrides (§4.6) are the one way to replace a dependency's requirement, only the project can declare them, and CPM shows wherever they apply.
- The lockfile's versions are kept whenever they still fit (§4.5). The backtracking solver of CPM 0.4.1 and earlier gives the same result whenever the lockfile pins every package. Without a lockfile, the two may choose different solutions, both valid. Both prefer higher versions, but they decide packages in a different order.

### 4.5 Interaction with the lockfile

| Command | Behaviour |
|---|---|
| `cpm install` (no args) | Locked versions are preferred and kept whenever they still satisfy the manifest. Only names whose root specifier changed, or that became unsatisfiable, are re-resolved. |
| `cpm install <pkg…>` | Like the above, but the named packages are unlocked and resolved to the newest version allowed by the new specifier. |
| `cpm install --frozen-lockfile` | The lock MUST be fresh ([lockfile.md §4](lockfile.md#4-freshness)). The solver does not run. Nothing is written. |
| `cpm remove <pkg…>` | The named entries are removed from the manifest. The remaining graph is re-resolved with every surviving lock entry pinned. Packages that are no longer reachable are dropped. |
| `cpm update [pkg…]` | Ignores lock preferences for the named packages, or for all of them, so they resolve to the newest versions their ranges allow. Git branches move to their current commit. The manifest is unchanged. |

### 4.6 Overrides

When the project knows that a dependency works with a version the dependency's range doesn't allow, it can say so in `"overrides"` in its `cpm.jsonc` ([manifest.md §3](manifest.md#3-field-reference)), instead of waiting for a new release of that dependency:

```jsonc
"overrides": {
  "fmt": "^10.2.0",         // every package's requirement on fmt
  "spdlog>fmt": "^10.2.0"   // only spdlog's requirement on fmt
}
```

- **What changes.** An override replaces the version range of other packages' requirements on a name. `"fmt"` replaces it in every package that requires fmt, and `"spdlog>fmt"` only in spdlog, whatever its version. The scoped form wins over the plain one. The override's range becomes the whole requirement: the dependency's own range no longer counts. A requirement that names a git or path source has no range, so overrides leave it alone.
- **What stays.**
  - There is still one fmt (§4.1).
  - Overrides don't touch the project's own `dependencies` and `devDependencies`, since the project states those directly.
  - They don't add a dependency: an override on a name that no package requires does nothing, and `cpm install` warns about it.
- **Never silent.**
  - Explanations (§4.3) mark an overridden requirement: `spdlog 1.12.0 depends on fmt ^10.2.0 (override)`. When one is involved in a conflict, the hint names the override too.
  - `cpm-lock.json` records the overrides, and for each package the requirements that an override replaced, with the package's own range ([lockfile.md §2](lockfile.md#2-schema)).
  - `cpm install` lists the replaced requirements every time it runs. `cpm why` shows the override next to the dependent's own range, and `cpm outdated` says when an override is no longer needed ([cli.md](cli.md#cpm-outdated)).
- **Only the project.** An override is the final project's decision, made with all of its dependencies in view. `cpm publish` refuses a package that declares overrides. When the manifest of a git or path dependency declares overrides, CPM ignores them with a warning.
- **No longer needed.** An override is no longer needed when every requirement it replaced, at its own range, allows the version that `cpm-lock.json` has, or when it replaced nothing. Removing it then changes no versions.

## 5. ODR policy and symbol isolation

- By default a diamond that needs two incompatible versions of one package is a **resolution error** (§4.3). CPM never links two versions of the same package into one binary on its own.
- **Escape hatch (*post-MVP*):** `"isolate": true` on a dependency's object form lets that dependency keep a private copy of its own static-library dependencies:
  1. The isolated package and its private dependencies are resolved as a separate sub-graph. They are exempt from the flat rule for the names involved.
  2. All of them are compiled with `-fvisibility=hidden -fvisibility-inlines-hidden`. The isolated package's own exported API is marked visible through its public headers, with its `<NAME>_API` ([manifest.md §4.1](manifest.md#41-exported-symbols-name_api)).
  3. The objects are combined with `ld -r` into one relocatable object, and `objcopy --localize-hidden` is applied. The result is packaged as `lib<name>.a`, so private symbols cannot collide in the final link.
  4. Isolation is refused when the isolated package's public headers include headers of a private dependency. Types from a private dependency would then cross the boundary and violate the ODR again. CPM checks this by scanning the include graph of the public headers.
  5. On MSVC, isolation requires building the isolated package as a DLL (*later*).
