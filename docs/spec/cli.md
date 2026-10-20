# Command-line interface

```
cpm [global options] <command> [command options] [arguments] [-- passthrough…]
```

## 1. Global options

Global options may appear before or after the command, but not after `--`.

| Option | Description |
|---|---|
| `-C, --dir <path>` | Run as if started in `<path>`. |
| `-v, --verbose` | More output. Repeat (`-vv`) to also print every subprocess command line. |
| `-q, --quiet` | Errors only. |
| `--color <auto\|always\|never>` | Colour output. `auto`: on when stderr is a terminal, `NO_COLOR` is unset and `TERM` is not `dumb`. |
| `-h, --help` | Help for CPM or for the given command. Printed to stdout. |
| `-V, --version` | Prints `cpm <version>` to stdout. With `-v` it adds the commit, the compiler CPM was built with, and `$CPM_HOME`. |

Short boolean flags can be combined (`-Dv`). Option values may be attached (`--profile=release`, `-j8`) or separate (`--profile release`, `-j 8`).

## 2. Exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Operation failed: resolution, network, integrity, compile or link error, failing test, invalid manifest |
| `2` | Usage error: unknown command or option, missing or invalid argument |
| `130` | Interrupted (Ctrl-C / SIGINT). Child processes are terminated, and staged store writes are abandoned, never half-committed. |
| *n* | `cpm run`: the exit code of the script or program, if CPM itself succeeded |

## 3. Commands

### `cpm init`

```
cpm init [<dir>] [-y] [--name <name>] [--lib | --shared-lib | --header-only] [--std <std>] [--force]
```

Scaffolds a package in `<dir>` (default `.`, created if missing).

- **Interactive** by default. It asks, in this order, for name, version (`0.1.0`), description, type (`executable`), C++ standard (`c++23`) and license, npm-style: `version: (0.1.0) `. Pressing Enter takes the default in parentheses. An invalid answer is explained and the question is asked again. At the end of input, the remaining defaults are taken. Values given as options (`--name`, `--lib`/`--shared-lib`/`--header-only`, `--std`) are not asked. The type question also accepts short answers: `exe`, `lib`/`static`, `shared`/`dll`, `header`. Prompts go to stderr. When stdin is not a terminal, `-y` is implied.
- **`-y`** accepts every default. The name is the directory name lowercased, with every run of invalid characters replaced by `-` and leading or trailing punctuation trimmed. If the result is still invalid ([manifest.md §3.1](manifest.md#31-package-names)), `--name` is required.
- **Never overwrites existing files.** An existing `cpm.jsonc` is an error unless `--force` is given. Other existing files are skipped with a notice.
- **Files created** (`<id>` is the name turned into a C++ identifier for the templates' namespace: `.` and `-` become `_`, a leading digit gets a `_` prefix, and a C++ keyword or `std` gets a `_` suffix):

| Type | Files |
|---|---|
| executable (default) | `cpm.jsonc`, `src/main.cpp`, `src/greeting.hpp`, `src/greeting.cpp`, `tests/greeting.test.cpp`, `.gitignore` |
| `--lib` / `--shared-lib` | `cpm.jsonc`, `include/<name>/<name>.hpp`, `src/lib.cpp`, `tests/<name>.test.cpp`, `.gitignore` |
| `--header-only` | `cpm.jsonc`, `include/<name>/<name>.hpp`, `tests/<name>.test.cpp`, `.gitignore` |

The generated `cpm.jsonc` carries comments that explain each field and the conventions. The executable template shows the core/entrypoint split ([manifest.md §4](manifest.md#4-target-inference)): `greeting.cpp` is shared by the executable and the test. The C++ templates only use C++11, so they build with every `std`. `.gitignore` contains `cpm_modules/`, `/build/` and `compile_commands.json`. If a `.gitignore` already exists, missing lines are appended to it.

Afterwards CPM lists the files it created and the ones it kept, and prints the next steps: `cd <dir>` (if needed), then `cpm run` (executables) or `cpm build`, then `cpm test`.

### `cpm install`

```
cpm install [<spec>…] [-D] [-E] [--frozen-lockfile] [--offline | --prefer-offline]
            [--registry <name|url>] [--profile <name> | --release] [--no-build]
aliases: i, add
```

Without specs, CPM installs everything in the manifest. With specs, it adds or updates those dependencies ([dependencies.md §1.4](dependencies.md#14-cli-specifiers-cpm-install-spec)).

| Option | Description |
|---|---|
| `-D, --save-dev` | Write new specs to `devDependencies`. |
| `-E, --save-exact` | Write the exact version instead of `^<version>` for bare names. |
| `--frozen-lockfile` | Fail if the lockfile is not fresh. Never writes the manifest or the lockfile. Not allowed together with specs. |
| `--offline` | Use only the caches and the store. Fail on anything that needs the network. |
| `--prefer-offline` | Use cached index documents of any age, and fetch only what is missing. |
| `--registry <name\|url>` | Registry for the registry specs given on this command line; they are written in the object form `{ "version": …, "registry": … }`. |
| `--profile <name>`, `--release` | Prepare binaries for this profile's ABI. Default `debug`. |
| `--no-build` | Resolve, fetch and link headers, and use prebuilt binaries the registry offers, but build nothing from source. `cpm build` builds what is missing later. |
| `--cxx <compiler>` | Compiler whose ABI the dependency binaries are built for ([build.md §1.1](build.md#11-selection-order)). |

A spec is a registry package (`fmt` adds `^<newest release>`, or the exact version with `-E`; `fmt@~10.1` keeps the range as typed), a path (`../mylib`, `file:../mylib`), a git URL (`git+https://host/repo.git#v1.2.0`, `github:owner/repo#v1.2.0`), or a path or git spec prefixed with a name (`mylib@file:../mylib`). A path or git spec without a name takes the name from the package's own `cpm.jsonc`; a package without one must be added by hand with a manifest overlay. Relative paths are resolved against the current directory and written relative to the project root. `--offline` and `--prefer-offline` exclude each other.

After a successful install CPM reports every manifest edit (`Updated cpm.jsonc: dependencies "mylib": "file:../mylib"`) and every package change: `+` added, `-` removed, `~` changed version or source.

**Pipeline** (each step is reported):

1. Load the manifest and the lockfile, and parse the specs.
2. Resolve ([dependencies.md §4](dependencies.md#4-resolution)), skipped when the lock is fresh and no specs were given.
3. Fetch missing sources into the store, verifying integrity ([store.md](store.md)).
4. Ensure binaries for the selected ABI: a prebuilt binary if available, otherwise a source build ([build.md §5](build.md#5-dependency-builds)).
5. Link `cpm_modules/`, remove stale package directories, and write `.cpm-modules.json`.
6. **Only after every earlier step succeeds:** apply the manifest edits ([manifest.md §10](manifest.md#10-edits-made-by-cpm)), then write `cpm-lock.json`.

If a step fails, the manifest and the lockfile stay untouched. Store entries that were completed are kept, since they are valid cache content.

### `cpm remove`

```
cpm remove <name>… [--offline] [--profile <name> | --release] [--cxx <compiler>]
aliases: rm, uninstall
```

Removes the named packages from `dependencies` and `devDependencies`, then re-runs the install pipeline with every remaining lock entry pinned ([dependencies.md §4.5](dependencies.md#45-interaction-with-the-lockfile)). Naming a package that is not a direct dependency is an error. Store entries are never deleted ([store.md §8](store.md#8-garbage-collection-post-mvp)).

### `cpm build`

```
cpm build [--profile <name> | --release] [-j <n>] [--tests] [--cxx <compiler>] [--clean]
```

Builds the root package ([build.md](build.md)). If the lockfile is stale or `cpm_modules` is out of date, it installs first and says so.

| Option | Description |
|---|---|
| `--profile <name>` | Profile to build (default `debug`). |
| `--release` | Same as `--profile release`. |
| `-j, --jobs <n>` | Parallel jobs passed to Ninja (default: Ninja's default). |
| `--tests` | Also build the test executables. |
| `--cxx <compiler>` | Compiler override ([build.md §1.1](build.md#11-selection-order)). |
| `--clean` | Delete `cpm_modules/.cache/<profile>/` and `build/<profile>/` before building. |

On success it prints the profile, the elapsed time, and the main output path, for example `build/debug/hello.exe` ([build.md §4.1](build.md#41-build-and-output-directories)).

### `cpm run`

```
cpm run [<script>] [--profile <name> | --release] [-j <n>] [--cxx <compiler>] [-- <args>…]
```

- **With `<script>`:** runs `scripts.<script>` from the manifest through the system shell, in the project root, without building first. The arguments after `--` reach the script exactly as given:
  - **Windows:** `%COMSPEC%` (normally `cmd.exe`) runs `/d /s /c "<script> <args>"`. Each argument is quoted with C runtime rules and then has cmd's metacharacters `( ) ! ^ " < > & |` escaped with `^`. `%` cannot be escaped on a cmd command line, so `%NAME%` inside an argument is still expanded.
  - **POSIX:** `/bin/sh -c '<script> "$@"' sh <args>`, so the shell never re-parses the arguments.

  A name that is not defined in `scripts` is an error, which lists the defined scripts.
- **Without `<script>`:** builds the root package and runs it with the arguments after `--`, in the **current directory** (like any program the user runs). A package that is not an executable is an error before anything is built.
- **Both:** stdin, stdout and stderr are inherited, and the exit code becomes CPM's. While the child runs, CPM ignores Ctrl-C and leaves it to the child. A child that ends because of Ctrl-C makes CPM exit with 130.
- **Environment** (scripts and the executable): the inherited environment, plus:

| Variable | Value |
|---|---|
| `PATH` | prefixed with `build/<profile>` and the directory of the running `cpm` |
| `CPM_PROJECT_ROOT` | absolute project root |
| `CPM_PROFILE` | active profile name |
| `CPM_PACKAGE_NAME`, `CPM_PACKAGE_VERSION` | from the manifest |
| `CPM_BIN_DIR` | `build/<profile>`, absolute |
| `CPM_EXE` | absolute path of the running `cpm` |

Paths in these variables use the OS's own separators. `pre<script>`/`post<script>` hooks are reserved and not run in v0.1.

### `cpm test`

```
cpm test [<filter>…] [--profile <name> | --release] [-j <n>] [--timeout <seconds>] [--no-run]
```

Builds and runs `tests/*.test.cpp` executables ([build.md §7](build.md#7-tests)). A filter selects tests whose name contains it. Several filters are ORed. `--no-run` only builds. Exits with 1 if any test fails.

### `cpm update`

```
cpm update [<name>…] [--profile <name> | --release] [--cxx <compiler>] [--no-build]
alias: up
```

Re-resolves without the lockfile's preferences for the named packages (direct or transitive), or for every package when no name is given, so each moves to the newest version its ranges allow. A git dependency on a branch moves to the branch's current commit. The manifest is not changed: to go beyond a range, edit it or run `cpm install <name>@<range>`. Naming a package that is not in `cpm-lock.json` is an error. Otherwise the pipeline is `cpm install`'s, including the report of every change.

### `cpm outdated`

```
cpm outdated
```

Lists the direct registry dependencies (`dependencies` and `devDependencies`) that have newer versions, like `npm outdated`:

| Column | Version |
|---|---|
| Current | the one in `cpm-lock.json` |
| Wanted | the newest the range in `cpm.jsonc` allows, which `cpm update` moves to unless another package constrains it |
| Latest | the newest on the registry, whatever the range |

Versions are compared the way resolution orders them ([dependencies.md §4.2](dependencies.md#42-algorithm)): yanked versions never count, a release comes before any prerelease, and a prerelease is Wanted only when the range allows it ([§3.3](dependencies.md#33-prereleases)) and Latest only when the package has no release. A dependency is listed when its three versions are not all the same, with `-` for a version that does not exist: Current when the lockfile has none, Wanted when the range allows no unyanked version. When nothing is listed, CPM says that every registry dependency is up to date.

```
$ cpm outdated
Package  Current  Wanted  Latest
fmt      11.1.4   11.2.0  12.2.0
spdlog   1.17.0   1.17.0  2.0.0
```

The table goes to stdout. Path and git dependencies are skipped with a note on stderr, since they have no registry versions to compare. Indexes are read like `cpm install` reads them ([registry.md §5](registry.md#5-client-behaviour)), and a dependency whose registry has no such package is an error. The exit code is 0 whether or not something is outdated.

### `cpm info`

```
cpm info <name>[@<range>] [--registry <name|url>]
```

Shows a package on a registry without installing it. Without `--registry` it uses the default registry. With a range, CPM shows the version resolution would pick for that range (the "Wanted" of `cpm outdated`). Without one, it shows the newest version (its "Latest"). On stdout:

```
$ cpm info spdlog
spdlog 1.17.0 · static library · C++11 · MIT
Fast C++ logging library, compiled, using the fmt package

repository    https://github.com/gabime/spdlog
owners        meirborowski
downloads     6
dependencies  fmt ^12.2.0
versions      1.17.0 (latest), 1.16.0, 1.15.3 (yanked)

install with  cpm install spdlog
```

- The first line has the version shown, its library type, its C++ standard and, when known, its license. Next comes the description, when known.
- The details come from the version shown: its dependencies and their ranges ("none" when it has none). The versions follow, newest first, with yanked ones marked. At most 20 are listed, followed by how many more there are.
- The index document ([registry.md §3](registry.md#3-index-documents)) gives the versions, types, standards and dependencies. A registry service ([registry.md §7.5](registry.md#75-browsing)) adds the description, license, repository, homepage (shown when it differs from the repository), owners and downloads. For a static registry those lines are left out. They are also left out, with a warning, when the service can't be reached.
- Long lists wrap at 80 columns, aligned under their first item. The last line is the command that adds the package, with the range and `--registry` when they were given.
- A package the registry doesn't have, and a range no unyanked version satisfies, are errors.

### `cpm tree`

```
cpm tree [--depth <n>] [--prod]
```

Prints the project's resolved dependencies from `cpm-lock.json` on stdout, as a tree. The project is at the top, and each package sits under the package that requires it:

```
$ cpm tree
hello 0.1.0
├── fmt 12.2.0
├── spdlog 1.17.0
│   └── fmt 12.2.0 (*)
└── doctest 2.4.11 (dev)
```

- Every package appears with the one version the project uses ([dependencies.md §4.1](dependencies.md#41-model)).
- A package already expanded higher up is marked `(*)` and not expanded again.
- Git and path dependencies show their source: `(git <url>#<ref>)` or `(path <path>)`. devDependencies are marked `(dev)`.
- `--depth <n>` shows `n` levels; `--depth 1` shows only direct dependencies. `--prod` leaves out devDependencies.
- Without a `cpm-lock.json` there's nothing to show, so it's an error that suggests `cpm install`. When the lockfile is out of date, a note on stderr says so.

### `cpm why`

```
cpm why <name>
```

Shows why a package is in the project: every chain of dependencies that leads from the project to it, read from `cpm-lock.json`. On stdout, as a tree that starts at the package and goes up through the packages that require it. Each step gives the range the dependent asks for:

```
$ cpm why fmt
fmt 12.2.0
├── ^12.2.0 from hello 0.1.0 (the project)
└── ^12.2.0 from spdlog 1.17.0
    └── ^1.17.0 from hello 0.1.0 (the project)
```

- The project's ranges come from its `cpm.jsonc`. A dependency's ranges come from its manifest: in the store, or in its directory for a path dependency. When that manifest isn't on this machine, for example before `cpm install` has run, the step shows the locked version instead of a range.
- devDependencies of the project are marked `(dev)`.
- A dependent already shown higher up is marked `(*)` and not followed again, so the output stays small in graphs with many shared dependencies.
- A package that isn't in `cpm-lock.json` is an error.

When a conflict stops `cpm install`, `cpm why` on the installed packages shows which dependencies hold the ranges involved.

### `cpm cache`

```
cpm cache
cpm cache clean [--dry-run] [--all]
```

`cpm cache` shows on stdout what CPM keeps in `$CPM_HOME` and the space it takes:
- the store's packages and the libraries built in it;
- the downloaded archives and the git mirrors;
- the projects CPM knows ([store.md §8](store.md#8-cleaning-up));
- how much of all that no known project uses.

```
$ cpm cache
home       C:/Users/me/.cpm
store      6 packages, 9 built libraries, 41.2 MB
downloads  6 archives, 3.1 MB
git        2 mirrors, 12.0 MB
projects   3
unused     2 packages, 1 archive, 5.4 MB (cpm cache clean removes them)
```

`cpm cache clean` removes the store entries, downloaded archives and git mirrors that no known project's `cpm-lock.json` uses, with stale temporary files. It also forgets projects whose directory or `cpm.jsonc` is gone. It reports what it removed and the space it freed.

- `--dry-run` lists what would be removed, and removes nothing.
- Something another `cpm` process is using at that moment, such as a package it is building, is kept, with a warning.
- `--all` empties the store, `cache/`, `tmp/` and `logs/`. It keeps the configuration, sign-ins, `tools/` (the compilers and Ninja from `cpm setup`), and CPM itself.

Removing something a project still needs costs a rebuild, never a broken project: its next `cpm install` or `cpm build` downloads and builds it again.

### `cpm publish`

```
cpm publish [--registry <name|url>] [--out <dir>] [--dry-run]
```

Packs the current package and adds its version to a registry ([registry.md §6](registry.md#6-publishing)). Without `--registry` it uses the default registry. A `file://` registry is written directly. Any other registry needs `--out <dir>`, which writes the archive and the updated index document there, to be uploaded to the registry's host. `--dry-run` runs every check and packs the archive, then lists the files and writes nothing. `--with-binary` is reserved and currently refused.

### `cpm setup`

```
cpm setup [--check] [--yes] [--managed]
```

Makes the machine ready to build: a C++ compiler CPM supports (GCC 13+, Clang 17+, Apple Clang 15+), Ninja 1.10+, and the helpers it drives (`git`, `curl`, `tar`). The one-line installers run it after installing CPM ([registry.md §8](registry.md#8-installing-cpm)). It first reports what it finds, then installs what is missing:

| Platform | Compiler | Ninja | git and curl |
|---|---|---|---|
| Windows | WinLibs GCC (MinGW-w64, UCRT), unpacked into `$CPM_HOME/tools` | downloaded into `$CPM_HOME/tools` | curl comes with Windows; git is not installed, and CPM prints how to get it |
| Linux | the distribution's packages (`apt-get`, `dnf`, `pacman`, `zypper`, `apk`, with `sudo` unless run as root), choosing the newest GCC package the distribution has | the distribution's package, or downloaded into `$CPM_HOME/tools` when there is no package manager | the distribution's packages (with `ca-certificates` on apt-get, which CPM runs without recommended packages) |
| macOS | Apple's Command Line Tools (`xcode-select --install`, which asks in a dialog; run `cpm setup` again afterwards) | downloaded into `$CPM_HOME/tools` | come with macOS and the Command Line Tools |

With `apt-get`, CPM installs a versioned GCC package such as `g++-14` only if `apt-cache policy` lists that exact package. apt reads a name no package has as a regular expression, so `g++-15` could otherwise match unrelated packages like `clang-15`.

- **Pinned downloads.** What goes into `$CPM_HOME/tools` is listed, with URLs and SHA-256 checksums, in the `toolchains.json` of CPM's built-in registry ([registry.md §8.2](registry.md#82-toolchainsjson)); `CPM_TOOLCHAINS_URL` replaces that location. Every download is verified before it is unpacked, unpacked into a staging directory, and moved into place only when complete.
- **Configuration.** When CPM installs a tool into `$CPM_HOME/tools`, or chooses a compiler that is not the first one on `PATH` (such as `g++-14`), it records it in `$CPM_HOME/config.jsonc` (`"cxx"`, `"ninja"`), editing the file in place so comments survive. Tools that are already usable are left alone.
- **Before changing anything** CPM lists the commands it will run and the downloads with their sizes, and asks. `--yes` answers yes (for scripts and CI). `--check` only reports, and exits with 1 if anything is missing.
- `--managed` installs the tools CPM can provide into `$CPM_HOME/tools` (on Windows the compiler and Ninja, elsewhere Ninja) even when usable ones are already installed, and points the configuration at them.
- **Last**, CPM checks the result the way a build would use it: it probes the compiler, checks that it accepts `-std=c++23`, and runs `ninja --version`.

### `cpm login`, `cpm logout`, `cpm whoami`

```
cpm login [--registry <name|url>] [--with-token]
cpm logout [--registry <name|url>]
cpm whoami [--registry <name|url>]
```

For registry services ([registry.md §7](registry.md#7-registry-service-api-v1)); the default registry unless `--registry` is given. `cpm login` signs in with GitHub's device flow: it prints a code and a URL, opens the URL in a browser when it can, waits for the user to approve, and saves the token in `$CPM_HOME/credentials.json`. `--with-token` reads a token from standard input instead (for CI, with a token created on the service's website). `cpm logout` revokes the token on the service and removes it locally. `cpm whoami` prints the signed-in account.

### `cpm yank`

```
cpm yank <name>@<version> [--undo] [--registry <name|url>]
```

Marks a published version as yanked, or with `--undo` reverses that ([registry.md §7.4](registry.md#74-yanking-and-owners)). Needs `cpm login` and ownership of the package.

### `cpm owner`

```
cpm owner list <name> [--registry <name|url>]
cpm owner add <name> <github-login> [--registry <name|url>]
cpm owner remove <name> <github-login> [--registry <name|url>]
```

Lists or changes the owners of a package on a registry service. Changing them needs ownership.

### `cpm search`

```
cpm search <query>… [--registry <name|url>]
```

Searches a registry service ([registry.md §7.5](registry.md#75-browsing)) for packages whose name or description contains the query; several words are searched as one query. Without `--registry` it uses the default registry. Each match prints on stdout as its name, right-aligned like the verbs of status lines, its latest version and its description, in the service's order:

```
$ cpm search log
      spdlog 1.17.0  Fast C++ logging library, compiled, using the fmt package
```

Only the first page of 20 matches is shown. When there are more, a note on stderr says how many. No match is not an error: CPM says so and exits with 0. A static registry (one whose `config.json` has no `api`) cannot be searched, so naming one is an error.

### Reserved command names (*post-MVP*)

`clean`, `fmt`, `workspace`, `toolchain`.

## 4. Output

Normative:

- CPM's status messages, warnings, errors and the build tool's output go to **stderr**.
- **stdout** is reserved for requested data: `--version`, `--help`, the results of `cpm search`, `cpm outdated`, `cpm info`, `cpm tree`, `cpm why`, `cpm cache` (without `clean`), `cpm whoami` and `cpm owner list`, and the output of programs and scripts started by `cpm run`/`cpm test -v`. `cpm run` can therefore be used in pipes.
- Diagnostics follow [architecture.md §5](architecture.md#5-errors-and-diagnostics).

Non-normative, recommended style (status verb right-aligned to 12 columns, bold green when colour is on):

```
$ cpm i fmt
   Resolving 2 packages
 Downloading fmt@10.2.1 (412 KiB)
    Building fmt@10.2.1 from source (abi 3f9a1c2b7d4e5a60)
      Linked 2 packages into cpm_modules

dependencies:
+ fmt 10.2.1
devDependencies:
+ doctest 2.4.11 (header-only)

Done in 6.2s

$ cpm build
   Compiling hello v0.1.0 (debug, gcc 13.2.0)
[4/4] LINK build/debug/hello.exe
    Finished debug in 1.21s → build/debug/hello.exe
```

Progress lines that rewrite themselves (`\r`) are used only when stderr is a terminal.

## 5. Configuration

User configuration lives in `$CPM_HOME/config.jsonc` (JSONC, optional):

```jsonc
{
  "registries": {
    "default": "https://registry.example.org/cpm/",
    "internal": { "url": "file:///C:/registries/internal/", "allowHttp": false }
  },
  "cxx": "g++",                       // compiler (build.md §1.1)
  "ninja": "C:/tools/ninja.exe",      // otherwise CPM_NINJA, then PATH
  "jobs": 0,                          // 0 = Ninja's default
  "color": "auto",
  "linkMode": "auto"                  // auto | symlink | junction | copy (store.md §6)
}
```

- A registry value is a URL string or an object `{ "url", "allowHttp" }`. Tokens for registry services are not part of the configuration: `cpm login` keeps them in `$CPM_HOME/credentials.json` ([registry.md §7.2](registry.md#72-accounts-and-tokens)), so `config.jsonc` can be shared safely.
- **Built-in default:** when no `default` registry is configured and `CPM_REGISTRY` is unset, CPM uses the URL built into it (`src/install/default_registry.hpp`). Since 0.2 that is the public registry service `https://cpm-registry.com/`.
- Unknown keys warn. Precedence for each setting: command-line option > environment variable > `config.jsonc` > built-in default.

## 6. Environment variables

| Variable | Effect |
|---|---|
| `CPM_HOME` | CPM home directory ([store.md §1](store.md#1-cpm-home)) |
| `CPM_REGISTRY` | URL that overrides the `default` registry |
| `CPM_TOOLCHAINS_URL` | URL of the `toolchains.json` that `cpm setup` reads (default: the built-in registry's) |
| `CPM_NINJA` | Path to `ninja` |
| `CXX` | Compiler ([build.md §1.1](build.md#11-selection-order)) |
| `CPM_KEEP_SANDBOX` | `1` keeps failed dependency-build sandboxes for inspection |
| `CPM_LOG` | `debug` or `trace`: internal diagnostics on stderr |
| `NO_COLOR` | Turns colour off (same as `--color never`) unless `--color always` is given |

Variables CPM **sets** for child processes: `CPM_PROJECT_ROOT`, `CPM_PROFILE`, `CPM_PACKAGE_NAME`, `CPM_PACKAGE_VERSION`, `CPM_BIN_DIR`, `CPM_EXE` (scripts), plus `CPM_TEST=1` (tests).
