#!/bin/sh
# Self-hosting check (docs/spec/architecture.md §7): stage 0 -> stage 1 -> stage 2.
#   stage 0: scripts/bootstrap.sh (one compiler call, no CPM)
#   stage 1: stage 0 runs `cpm build --release`
#   stage 2: stage 1 rebuilds CPM from a clean build directory; stage 2 must pass `cpm test`
set -eu
cd "$(dirname "$0")/.."

cache=cpm_modules/.cache
stage0=$cache/bootstrap/cpm
stage1=$cache/stage1/cpm
release=build/release/cpm

step() { printf '\n== %s\n' "$1"; }

if [ "${1:-}" != "--skip-bootstrap" ]; then
    step 'stage 0: bootstrap'
    sh scripts/bootstrap.sh
fi

step 'stage 1: stage 0 builds CPM (release, clean)'
"$stage0" build --release --clean
mkdir -p "$(dirname "$stage1")"
cp "$release" "$stage1"

step 'stage 2: stage 1 rebuilds CPM from clean'
"$stage1" build --release --clean

step 'stage 2: --version'
"$release" --version

step 'stage 2: cpm test'
"$release" test

step 'stage 2: cpm init -y && cpm run in an empty directory'
rm -rf "$cache/smoke"
mkdir -p "$cache/smoke"
"$release" -C "$cache/smoke" init -y hello
greeting=$("$release" -C "$cache/smoke/hello" run 2>/dev/null)
[ "$greeting" = "Hello, world!" ] || { echo "cpm run printed '$greeting'" >&2; exit 1; }
echo "cpm run printed: $greeting"

step 'stage 2: no-op build'
"$release" build --release

step 'stage 2 rebuilds its own executable'
touch src/version.hpp
"$release" build --release
"$release" --version

printf '\nself-hosting OK\n'
