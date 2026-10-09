# gophper-wasm

PHP compiled to wasm32-wasip1 for gophper. See README.md for the layout and ABI.md for the host contract.

## Rules

- Do not commit `php-src/`. Change it there, then run `scripts/export-patches.sh`.
- Every php-src change carries a `GOPHPER:` marker comment. The export script refuses changes without one.
- Prefer `compat/` over patching php-src. A missing POSIX symbol belongs in `compat/`.
- `*.wasm.gz`, `ext/*.gz` and `version.go` are build outputs, ignored on branches. Only a release tag carries them, so the Go package builds only after `scripts/` ran.
- Release only through the `build` workflow. Never tag by hand, and never move, delete or reuse a pushed tag. The module proxy and the checksum database keep every version forever.
- For now main is one commit. Fold each change into it with `git commit --amend` and `git push --force`.
- Any change to ABI.md bumps `ABIVersion`, and gophper's `engineABIVersion` must follow.

## Skills

Read the matching one in `.agents/skills/` before you start. Each lists the designs already rejected in its area, and why.

| Skill | When |
| --- | --- |
| `release` | Releasing a version |
| `php-upgrade` | Moving to a new php-src release |
| `dependency-upgrade` | Updating a library, the toolchain or the CA bundle |
| `php-src-patch` | Changing php-src, or a patch that stopped applying |
| `compat-layer` | A POSIX function WASI lacks, `--wrap`, configure cache variables |
| `c-libraries` | Adding a library or an extension, or changing how one is built |
| `abi-change` | A host function, an exported global, or a shared constant |

## Before you finish

```sh
scripts/build.sh && scripts/build-ext.sh && go vet ./... && go test ./...
```

Then run gophper's tests against this checkout, through a `go.work` that uses both.

## Rejected designs

| Idea | Why not |
| --- | --- |
| Transpile php-src to Go with ccgo (modernc style) | ccgo is C only. ICU (`intl`) is C++, so `intl` would be impossible. |

## Known caveats

- `patches/0012` fixes a bug in php-src's `ext/openssl/xp_ssl.c`, new in PHP 8.6 and unfixed upstream on 2026-10-10: a failed `listen()` on a `tcp://` server looked successful. Drop the patch once php-src fixes it.
- wazero's exception handling is experimental. wazero issue #2522 (`exnref` use-after-free) was open on 2026-10-09.
- `sigsetjmp`/`siglongjmp` are macros for `setjmp`/`longjmp` in `compat/gophper_compat.h`. LLVM's sjlj lowering only knows the plain names.
