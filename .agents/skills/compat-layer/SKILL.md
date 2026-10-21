---
name: compat-layer
description: Add or change a POSIX function that WASI lacks, in gophper-wasm's compat/ (sockets, pipes, processes, signals, users, locks, mmap, fcntl, DNS). Use before editing compat/, COMPAT_WRAPS or COMPAT_SOURCES, or configure cache variables in scripts/build.sh.
---

# The compat layer

`compat/` gives php-src and the libraries what wasi-libc lacks. It is linked into php.wasm, and `gophper_compat.h` is force-included in every compilation.

| File | Covers |
| --- | --- |
| `gophper_compat.c` | Small stubs and wrappers: syslog, madvise, mmap, terminals |
| `gophper_net.c` | Sockets, `poll`, `select`, `fcntl`, `close` for every fd |
| `gophper_fd.c` | `dup` aliases |
| `gophper_proc.c` | `posix_spawn`, `waitpid`, `kill`, pipes to children |
| `gophper_signal.c` | Handlers, masks, `alarm` |
| `gophper_sys.c` | Users, `flock`, `stat` owners, `chmod`, host name, rlimits |
| `gophper_dns.c` | `res_search`, `dn_expand` |
| `gophper_dl.c` | `dlopen` through the host |
| `gophper_fn.c` | PHP functions written in Go |
| `include/` | Headers WASI lacks, such as `sys/wait.h` |

## Steps

1. Put the function in the file of its concern. A new file goes in `COMPAT_SOURCES` in `scripts/env.sh`.
2. To take over a libc function, define `__wrap_NAME`, call `__real_NAME` for the rest, and add NAME to `COMPAT_WRAPS`. A side module importing NAME gets `__wrap_NAME` automatically: gophper's dylink prefers that export.
3. Resolve fds first with `GOPHPER_RESOLVE(fd)`, so that a `dup` of a socket is that socket.
4. Tell configure about it. configure never sees compat:

   | Added | Announce in `scripts/build.sh` |
   | --- | --- |
   | A function | `export ac_cv_func_NAME=yes` |
   | A header in `include/` | `export ac_cv_header_NAME_h=yes`. Without it, PHP compiles the code that needs it out. |
   | A PHP-specific check | Its `php_cv_*` variable, from `grep -rn php_cv_ php-src/*.m4 php-src/ext/*/config.m4` |
   | A function `PHP_CHECK_FUNC` checks | A patch (`patches/0007`). That macro ignores cache variables. |

5. A library's configure needs it too: `compat_link` in `scripts/deps.sh` links its tests against compat.
6. If it needs the host, it is an ABI change (abi-change skill).
7. Build: `scripts/build.sh` runs `make clean` when compat changed. A change to `gophper_compat.h` or `include/` also rebuilds every library in `scripts/deps.sh`, since they must agree on types.

> [!WARNING]
> A wrapper that pretends to succeed can hide a real failure from PHP or a library.
> Ask who else calls it. Record locks that always succeeded let two SQLite writers corrupt a database.

## Rejected designs

| Idea | Why not |
| --- | --- |
| Add compat to the link during `configure` | configure would detect stub functions as real (`HAVE_*`). compat is linked only at `make` time. Real implementations are announced through cache variables in `scripts/build.sh`. |
| Trusting configure's `getrusage`, `mmap` and `mprotect` checks | Its test programs call them with the wrong prototype, and wasm-opt fails to parse the result. Check `config.log` for `error parsing wasm`. `getrusage` is announced, and the mman pair stays off. |
| Cache variables for `res_search`, `dn_expand` and `dn_skipname` | PHP's `PHP_CHECK_FUNC` unsets them and checks again. `patches/0007` defines `HAVE_*` for WASI instead. |
| `-Wl,--whole-archive -lc` to export all of libc | wasm-ld reads an archive once, clang passes `-lc` already, and libtool moves archives away from `-Wl` flags. Nothing was added, and `srand` was missing for zip.so. `scripts/build.sh` passes a copy of libc.a inside one `-Wl`, minus the members compat/ replaces. |
| opcache without shared memory | configure found no SHM backend, so a php-cgi worker compiled every script again on each request. `php_cv_shm_mmap_anon=yes` enables the mmap one. `compat/` turns its shared anonymous mapping into a private one. |
| Record locks that always succeed, on every file | opcache needs them on its lock file, but SQLite's `unix` VFS uses them too, and a script picks it with `?vfs=unix`. Two instances then both held `BEGIN EXCLUSIVE` on one database. `patches/0011` registers opcache's lock fd, and only its locks succeed. Others fail, as in wasi-libc. |
| Leaving `socket()` declared only in the force-included header | curl's configure also greps headers for prototypes. `compat/include/sys/socket.h` declares them, as Linux's does. |
| wasi-libc's `sigset_t` | It is one byte, so `sigaddset()` failed for signals above 8. `gophper_compat.h` defines musl's 128-byte one before any header. The libraries must agree on such types, so `scripts/deps.sh` rebuilds them when the compat headers change. |
| Installing the signal hook only through `zend_interrupt_function` at startup | pcntl wraps it in MINIT and calls it after dispatching, so a handler ran one check late. `sigaction()` puts compat's hook in front again. |
| Leaving configure to find `compat/include` | It does not look there. Without `ac_cv_header_sys_wait_h=yes`, PHP compiled `proc_close()` and `proc_get_status()` without `waitpid`, and they always returned -1. Announce every header added there. |
| Reading a pipe ahead as soon as it exists | The end may go to a child process, and reading ahead would take the child's input. The host starts reading an end only when PHP reads or polls it. |
| Sockets as host files on a wazero sysfs mount | wazero's `poll_oneoff` reports non-blocking host files as always ready, and blocking ones by stdin. `compat/gophper_net.c` wraps the calls with `--wrap` instead. |
| wasi-libc's `dup` and `dup2` | `dup` fails, and `dup2` moves the fd with `fd_renumber`, closing the old one. `compat/gophper_fd.c` keeps aliases instead. php://stdout needs it. |
| Wrapping `pread`, `pwrite`, `preadv`, `pwritev` and `posix_fadvise` | The linked module failed wasm-opt's validation (call parameter types). Not investigated. They stay unwrapped, so they do not resolve dup aliases. |
