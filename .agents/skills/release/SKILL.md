---
name: release
description: Release a new gophper-wasm version, then move gophper to it. Use when asked to release, tag, or publish the binaries, or when gophper needs binaries built from newer sources.
---

# Release

Only the `build` workflow releases. It builds and tests from scratch, makes an **orphan commit** of the sources and the binaries, and pushes only its tag. Branches never hold binaries.

## Steps

1. Push the sources to main. For now main is one commit: `git commit --amend` and `git push --force`.
2. Pick the version. The workflow also publishes a GitHub release titled `vX.Y.Z (PHP x.y.z)`, a pre-release for a PHP alpha, beta or RC. Never reuse one that was pushed.

   | Change since the last release | Bump |
   | --- | --- |
   | `ABIVersion` | Minor, while below v1 |
   | Anything else, such as a php-src or library update | Patch |

3. Run the release and wait for it (about 20 minutes with warm caches, 40 cold):

   ```sh
   gh workflow run build.yml -R mpyw/gophper-wasm --ref main -f version=vX.Y.Z
   gh run watch -R mpyw/gophper-wasm "$(gh run list -R mpyw/gophper-wasm -L 1 --json databaseId -q '.[0].databaseId')" --exit-status
   ```

   It fails before building if the version is malformed or exists already.
4. In gophper, follow its `gophper-wasm-upgrade` skill.

> [!CAUTION]
> A pushed tag is permanent. The first `go get` puts the version in proxy.golang.org and sum.golang.org, which keep it forever, and every Go module cache keeps it too.
> Never move, delete or reuse one. A broken release is fixed by the next version.

## Rejected designs

| Idea | Why not |
| --- | --- |
| Binaries built on a developer's Mac and committed by hand | Nothing proved they came from the committed sources. CI builds them instead. |
| Checking that CI rebuilds the committed binaries byte for byte | bison, re2c and the host OS shape the output, and OpenSSL embeds the compiler path. CI being the only builder needs no such check. |
| Committing the binaries to main on every push | Each source change added about 20 MB, and the repository reached 231 MB in 28 commits. A release tag carries them instead, on an orphan commit. |
| Release tags on commits that main contains | A clone would still fetch every release's binaries through main's history. Orphan commits keep them out of branches. |
| Git LFS for the wasm binaries | `go get` and the module proxy fetch only the LFS pointer files, so `go:embed` would embed text. |
