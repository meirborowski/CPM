#!/bin/sh
# Prints a version's release notes: its section of CHANGELOG.md, without the heading.
#
#   sh scripts/release-notes.sh 0.4.0              # the section
#   sh scripts/release-notes.sh 0.4.0 --install    # plus how to install and a link to the full changelog
#
# The release workflow publishes the --install form as the GitHub release's notes, and fails before building when
# a version has no section (or an empty one), so every release has notes.
set -eu
version="${1:?usage: release-notes.sh <version> [--install]}"
cd "$(dirname "$0")/.."

# "## 0.4.0 — 2026-10-19" starts a section; the next "## " ends it.
notes=$(awk -v v="$version" '
  /^## / { if (found) exit; if ($2 == v) { found = 1; next } }
  found { print }
' CHANGELOG.md)
if [ -z "$(printf '%s' "$notes" | tr -d '[:space:]')" ]; then
  echo "error: CHANGELOG.md has no release notes for $version; add a \"## $version — <date>\" section" >&2
  exit 1
fi
printf '%s\n' "$notes" | sed '/./,$!d'

[ "${2:-}" = --install ] || exit 0
repo="${GITHUB_REPOSITORY:-meirborowski/CPM}"
cat <<'EOF'

## Install

Linux and macOS:

```sh
curl -fsSL https://cpm-registry.com/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://cpm-registry.com/install.ps1 | iex
```

The installers get the newest release, then run `cpm setup`, which installs a C++ compiler and Ninja if you need them. Running one again updates CPM. To install this version by hand, download the file for your platform below and check it against `SHA256SUMS`.
EOF
previous=$(git describe --tags --abbrev=0 --match 'v*' "v$version^" 2>/dev/null || true)
if [ -n "$previous" ]; then
  printf '\n**Full changelog:** https://github.com/%s/compare/%s...v%s\n' "$repo" "$previous" "$version"
fi
