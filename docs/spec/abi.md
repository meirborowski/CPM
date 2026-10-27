# ABI fingerprint

Compiled artifacts can be reused only by consumers whose toolchain and settings produce compatible binaries. CPM captures those settings in an **ABI fingerprint**:
a set of `key=value` inputs and their SHA-256. The first 16 hex characters of the hash name the store directory `lib/<abi16>/` and the registry `binaries` key.

## 1. Inputs

Inputs marked "always" are present for every toolchain. Values are lowercase ASCII.

| Key | When | Values / source |
|---|---|---|
| `abi.scheme` | always | `2`. Bumped whenever this table changes meaning, which invalidates every cached binary. Scheme `2` (CPM 0.4.5) compiles `.c` sources as C. Scheme `1` compiled them as C++, so a package with `.c` sources built under it can differ. |
| `os` | always | `windows`, `linux`, `macos`, `freebsd` (CPM's compile-time platform, cross-checked with the target triple) |
| `arch` | always | `x86_64`, `aarch64`, `x86`, `arm` (from the target triple) |
| `target` | always | The compiler's target triple: `-dumpmachine` (GCC/Clang). For MSVC: `<arch>-pc-windows-msvc` |
| `compiler` | always | `gcc`, `clang`, `apple-clang`, `msvc`, and `clang-cl` (reserved: CPM doesn't build with it yet) |
| `compiler.version` | always | GCC: `-dumpfullversion` (`13.2.0`). Clang: `__clang_major__.__clang_minor__.__clang_patchlevel__`. MSVC: `_MSC_FULL_VER` (`195136260`), which changes with every toolset update, so it covers the toolset version |
| `stdlib` | always | `libstdc++`, `libc++`, `msvc-stl` (from `__GLIBCXX__`, `_LIBCPP_VERSION`, `_MSVC_STL_VERSION`) |
| `stdlib.abi` | always | libstdc++: `cxx11` if `_GLIBCXX_USE_CXX11_ABI` = 1, else `cxx98`. libc++: `v<_LIBCPP_ABI_VERSION>`. msvc-stl: `<_MSVC_STL_VERSION>` |
| `crt` | always | MinGW: `ucrt` (if `_UCRT` is defined) or `msvcrt`. Linux: `glibc` (`__GLIBC__`) or `musl`. macOS: `libsystem`. MSVC: `ucrt` |
| `eh` | GCC/Clang | Exception model: `seh` (`__SEH__`), `dwarf`, `sjlj` (`__USING_SJLJ_EXCEPTIONS__`), `itanium` (everything non-Windows) |
| `threads` | MinGW | `posix`, `win32` or `mcf` (from `-v` → `Thread model:`) |
| `std` | always | The root package's `std` (`c++23`) |
| `runtime` | always | `dynamic` or `static` (profile `runtime`) |
| `debug-runtime` | MSVC | `true` or `false` (profile `debugRuntime`) |
| `pic` | always | `true` on Linux, macOS and FreeBSD (dependencies are always built PIC there). `false` on Windows |
| `sanitizers` | always | Sorted, comma-separated effective sanitizers, or empty |
| `defines.abi` | always | Sorted, comma-separated ABI-affecting definitions from the root profile and manifest (§2), or empty |
| `exceptions` | always | `on`, or `off` when the root's `cxxflags` contain `-fno-exceptions` (MSVC: `/EHs-c-`, which overrides the `/EHsc` CPM always passes) |
| `rtti` | always | `on`, or `off` when the root's `cxxflags` contain `-fno-rtti` / `/GR-` |

**Not included,** because these settings do not affect binary compatibility of static code: optimisation level, debug info, warnings, `werror`, LTO (dependencies are never LTO-compiled), and non-ABI definitions.

**C sources add no inputs.** The C compiler always reports the same family, version and target as the C++ compiler ([build.md §1.2](build.md#12-family-and-companion-tools)), so `compiler`, `compiler.version` and `target` describe both, and `crt` is the C runtime both use. `cstd` isn't an input either: each package compiles its C code with its own `cstd`, whatever the consumer uses, and C's ABI doesn't depend on it ([manifest.md §3.3](manifest.md#33-cstd-semantics)). A pure C library's fingerprint still has the C++ inputs (`std`, `stdlib`, `exceptions` and so on). Changing them rebuilds it, which costs time but never gives a wrong binary.

## 2. ABI-affecting definitions

A definition counts toward `defines.abi` if its name (the part before `=`) is in this list:

```
_GLIBCXX_DEBUG  _GLIBCXX_DEBUG_PEDANTIC  _GLIBCXX_USE_CXX11_ABI  _GLIBCXX_FULLY_DYNAMIC_STRING
_LIBCPP_ABI_VERSION  _LIBCPP_ABI_UNSTABLE  _LIBCPP_HARDENING_MODE
_ITERATOR_DEBUG_LEVEL  _HAS_ITERATOR_DEBUGGING  _SECURE_SCL  _HAS_EXCEPTIONS
```

or if it starts with `_LIBCPP_ABI_`. The list is part of `abi.scheme` `2`, unchanged from `1`.

## 3. Canonical encoding and hash

1. Collect the inputs into a map. Every key in §1 that applies to the toolchain MUST be present; its value may be empty.
2. Serialise as UTF-8 lines `key=value\n`, sorted by key (byte-wise). Keys never contain `=` (a line is split at its first `=`), and values never contain `\n`; an input that would break this is rejected. Values may contain `=`, as in `defines.abi=_GLIBCXX_USE_CXX11_ABI=0`.
3. `hash` = lowercase hex SHA-256 of the serialised bytes (64 characters).
4. `abi16` = the first 16 characters of `hash`. This names `lib/<abi16>/` in the store and the `binaries` keys in the registry.
5. The full inputs are stored in `meta.json` and compared on every use ([store.md §2.2](store.md#22-libabi16metajson)). A 64-bit prefix is collision-safe in practice, and the comparison catches any collision that does happen.

**Example** for this host (MinGW-w64 g++ 13.2.0, UCRT, posix threads, SEH, `std: c++23`, debug profile):

```
abi.scheme=2
arch=x86_64
compiler=gcc
compiler.version=13.2.0
crt=ucrt
defines.abi=
eh=seh
exceptions=on
os=windows
pic=false
rtti=on
runtime=dynamic
sanitizers=
std=c++23
stdlib=libstdc++
stdlib.abi=cxx11
target=x86_64-w64-mingw32
threads=posix
```

The same project built with Visual Studio Community 2026's MSVC 19.51 would have:

```
abi.scheme=2
arch=x86_64
compiler=msvc
compiler.version=195136260
crt=ucrt
debug-runtime=false
defines.abi=
exceptions=on
os=windows
pic=false
rtti=on
runtime=dynamic
sanitizers=
std=c++23
stdlib=msvc-stl
stdlib.abi=145
target=x86_64-pc-windows-msvc
```

## 4. Probing

Fingerprint inputs are gathered without compiling or linking anything:

| Probe | Gives |
|---|---|
| `<cxx> -dumpfullversion` and `-dumpmachine` | `compiler.version`, `target` → `arch`, `os` |
| `<cxx> -v` (stderr) | `Thread model:` → `threads`; confirms the family (`gcc version`, `clang version`, `Apple clang`) |
| `<cxx> -std=<std> -x c++ -E -dM <probe.cpp>` | predefined and library macros: `__clang__`, `__GNUC__`, `__GLIBCXX__`, `_GLIBCXX_USE_CXX11_ABI`, `_LIBCPP_VERSION`, `_LIBCPP_ABI_VERSION`, `_UCRT`, `__GLIBC__`, `__SEH__`, `__USING_SJLJ_EXCEPTIONS__` |
| MSVC: `cl /nologo /EP /Zc:preprocessor /TP probe.cpp`, in the environment of [build.md §1.5](build.md#15-msvcs-environment). The probe prints each macro after a marker of its own, so that its lines can be told from the expanded headers | `_MSC_FULL_VER` (also the displayed version: `195136260` is `19.51.36260`), `_MSVC_STL_VERSION`, `_M_X64` / `_M_ARM64` → `arch` |
| MSVC: `cl /nologo /c /Zs /showIncludes` on a file that includes a header of its own | the text before the header's path, recorded as `depsPrefix` ([build.md §1.3](build.md#13-probe-cache)) |

`probe.cpp` is generated in `$CPM_HOME/tmp/` and contains only `#include <version>` and `#include <cstdlib>`. Probe results, minus the profile-dependent keys (`std`, `runtime`, `debug-runtime`, `sanitizers`, `defines.abi`, `exceptions`, `rtti`), are cached in `toolchains.json` ([build.md §1.3](build.md#13-probe-cache)). Building the fingerprint after the first run costs no subprocesses.

## 5. Where the fingerprint is used

- **Store:** `store/<entry>/lib/<abi16>/`.
- **Registry binaries:** `versions[v].binaries[<abi16>]`.
- **Dependency builds:** the sandbox build uses exactly the settings encoded in the fingerprint ([build.md §5](build.md#5-dependency-builds)), so artifacts always match their directory.
- **cpm_modules:** `.cpm-modules.json` → `abi` records which ABI the `lib` links point to.

On GCC and Clang the `debug` and `release` profiles normally produce the **same** fingerprint, because optimisation and debug info are excluded. Switching profiles therefore rebuilds no dependency. Sanitizers, `runtime`, ABI definitions, and on MSVC `debugRuntime`, do produce a new fingerprint.
