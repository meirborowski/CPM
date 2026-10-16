# Spec examples

A consistent set of example files. Every hash, commit and URL in them is illustrative.

| File | Shows |
|---|---|
| [hello/cpm.jsonc](hello/cpm.jsonc) | A root manifest with registry, git (with an overlay) and path dependencies, devDependencies, profiles and platform overrides |
| [hello/cpm-lock.json](hello/cpm-lock.json) | The lockfile for that manifest, in canonical byte format ([lockfile.md §3](../lockfile.md#3-serialization-normative)) |
| [mylib/cpm.jsonc](mylib/cpm.jsonc) | The path dependency `file:../mylib` |
| [registry/](registry) | A static registry: `config.json` plus the index documents for `fmt` and `doctest` |

These files are kept consistent with one another: the lockfile is fresh for the manifest, the index and the lock agree on integrity, and every file validates against [`/schema`](../../../schema).
