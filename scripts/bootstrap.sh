#!/bin/sh
# Stage 0: build CPM without CPM (docs/spec/architecture.md §7).
# Compiles every src/**/*.cpp in one compiler call. After this, use the result to build CPM with itself:
#   cpm_modules/.cache/bootstrap/cpm build --release
set -eu
cd "$(dirname "$0")/.."

CXX="${CXX:-c++}"
out=cpm_modules/.cache/bootstrap
mkdir -p "$out"

sources=$(find src -type f -name '*.cpp' | LC_ALL=C sort)
count=$(printf '%s\n' "$sources" | wc -l | tr -d ' ')
echo "bootstrap: $CXX -std=c++23 -O1 -Isrc ($count sources) -> $out/cpm"
# shellcheck disable=SC2086
"$CXX" -std=c++23 -O1 -Isrc $sources -o "$out/cpm" -pthread
"$out/cpm" --version
