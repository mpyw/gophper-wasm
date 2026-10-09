---
name: php-src-patch
description: Change php-src for gophper-wasm, through patches/. Use before editing anything under php-src/, adding or fixing a patch, or when a patch stops applying.
---

# Patch php-src

`php-src/` is a clone, ignored by Git. `patches/` holds the changes, one patch per php-src file.

> [!IMPORTANT]
> Prefer `compat/` (compat-layer skill). Patch php-src only for what compat cannot reach: PHP's own logic, or a call into the host at a point only PHP knows.

## Steps

1. Edit the file in `php-src/`. Wrap the change in `#ifdef __wasi__` when it is C, and mark it with a comment that starts with `GOPHPER:` and says why.
2. A call into compat or the host declares its function locally, as `patches/0010` does, so that no compat header changes. A header change rebuilds every library.
3. A file without a patch yet needs a name: add it to `patch_name()` in `scripts/export-patches.sh`, with the next number.
4. Run `scripts/export-patches.sh`. It writes every patch again and refuses a change without a `GOPHPER:` marker.
5. Build: `scripts/build.sh`. A change to `configure.ac` or a `config.m4` needs `scripts/build.sh --reconfigure`.
6. Check that the patches apply to a fresh clone: `scripts/fetch.sh --force`, then build again.

## Rejected designs

| Idea | Why not |
| --- | --- |
| Legacy wasm EH encoding (wasi-sdk default) | wazero rejects it. Keep `-mllvm -wasm-use-legacy-eh=false`. |
| Asyncify or snapshot/restore for setjmp | Not needed since wazero v1.12.0 has exception handling. |
| `USE_ZEND_ALLOC=0` to silence `munmap() failed` | Loses `memory_limit`. wasi-emulated-mman cannot align or partially unmap. `patches/0003` uses `aligned_alloc()` instead. |
| Fibers via ucontext / boost asm | WASI has neither. `patches/0002` makes `Fiber::start()` throw. |
| Registering Go functions from an extension's MINIT | Not every binary would load such an extension. `patches/0010` calls `gophper_fn_register()` from `php_module_startup()`, after the extensions, in both SAPIs. |
| php.ini through `-d` for php-cgi | php-cgi skips its arguments when `QUERY_STRING` starts with `-`. `--with-config-file-path=/etc/gophper` is used instead. |
| `/dev/urandom` for `random_bytes()` | PHP picks it on WASI, and PHP sees no `/dev` under a server's mounts. `patches/0005` reads WASI's `random_get`. |
