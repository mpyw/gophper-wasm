# ABI

The contract between these binaries and their Go host, [gophper](https://github.com/mpyw/gophper).
`phpwasm.ABIVersion` numbers it.

> [!IMPORTANT]
> Change `ABIVersion` whenever anything on this page changes.
> Then update the host to match. It refuses binaries with another `ABIVersion`.

## Host functions

All imports are from the module `gophper`.
A function that returns an errno returns `0` on success and a WASI errno otherwise.
A function that returns a count returns `-errno` on failure.

### Timer

| Function | Signature | Meaning |
| --- | --- | --- |
| `set_timeout` | `(seconds i32)` | Arm the execution timer. `0` cancels it. Called by `patches/0004`. |

When the timer fires, the host does what `zend_timeout_handler()` does.
It sets the bytes behind `gophper_timed_out` and `gophper_vm_interrupt`.
If `gophper_hard_timeout` is nonzero, it arms the timer again for that many seconds.

### Signals

The guest side is `compat/gophper_signal.c`. It keeps the handlers, the blocked mask and the pending set.
Signals are numbered as on Linux. A signal set is an i64 with bit `n` for signal `n`.

| Function | Signature | Returns |
| --- | --- | --- |
| `sig_watch` | `(handled, ignored i64)` | Nothing. The signals with a handler, and the ignored ones. |
| `sig_take` | `()` | The signals the host marked pending, as an i64. They are no longer pending. |
| `sig_wait` | `(mask i64, timeout_ms i32)` | `1` once a signal in `mask` is pending, `0` on timeout, or `-errno` |
| `alarm` | `(seconds i32)` | The seconds the previous alarm had left. `0` cancels it. |

To deliver a signal, the host marks it pending and sets `gophper_vm_interrupt`'s byte.
`patches/0008` makes the guest call `sig_take` at the next interrupt check, and run the handler or the default action.
A default action that ends the process exits with `128` plus the signal number.
`timeout_ms` below `0` waits forever. A blocking `sig_wait` returns `EINTR` on an interrupt and `EIO` once the run is over.

### Sockets

The guest side is `compat/gophper_net.c`. `fd` is the guest fd the socket owns.

| Function | Signature | Returns |
| --- | --- | --- |
| `sock_open` | `(fd, kind i32)` | errno |
| `sock_close` | `(fd i32)` | errno |
| `sock_connect` | `(fd i32, addr, addr_len u32, nonblock i32)` | errno. `EINPROGRESS` when non-blocking. |
| `sock_bind` | `(fd i32, addr, addr_len u32)` | errno |
| `sock_listen` | `(fd, backlog i32)` | errno |
| `sock_accept` | `(fd, new_fd, nonblock i32)` | errno |
| `sock_recv` | `(fd i32, buf u32, len, flags i32, from, from_cap, from_len u32)` | count |
| `sock_send` | `(fd i32, buf u32, len, flags i32, to, to_len u32)` | count |
| `sock_shutdown` | `(fd, how i32)` | errno. `how`: 1 read, 2 write, 3 both. |
| `sock_name` | `(fd, peer i32, out, out_cap, out_len u32)` | errno |
| `sock_getopt` | `(fd, opt i32, value u32)` | errno |
| `sock_setopt` | `(fd, opt, value i32)` | errno |
| `sock_poll` | `(fds, events, revents u32, n, timeout_ms i32)` | count. Three arrays of `n` i32. |
| `sock_available` | `(fd i32)` | count of bytes ready |
| `sock_pair` | `(a, b i32)` | errno. Makes `a` and `b` a connected pair of Unix stream sockets, as `socketpair(2)` does. Needs a Unix host. |

Addresses are text: `1.2.3.4:80`, `[::1]:80`, or a Unix socket path.
`from_len` and `out_len` point to a u32 the host writes.

| Constant | Values |
| --- | --- |
| `kind` | 1 TCP, 2 UDP, 3 Unix stream, 4 Unix datagram |
| `flags` | 1 peek, 2 do not wait |
| poll bits | 1 in, 2 out, 4 hang-up, 8 error |
| `opt` | 1 `SO_ERROR`, 2 `SO_TYPE`, 3 `TCP_NODELAY`, 4 `SO_KEEPALIVE`, 5 `SO_REUSEADDR`, 6 `SO_BROADCAST`, 7 `SO_RCVBUF`, 8 `SO_SNDBUF` |

A blocking call returns `EINTR` when an interrupt arrives. It returns `EIO` once the run is over.

### Dynamic linking

The guest side is `compat/gophper_dl.c`, which implements `dlopen(3)` and friends.
PHP loads extensions through them, for `extension=` and `dl()`.

| Function | Signature | Returns |
| --- | --- | --- |
| `dl_open` | `(wasm, len u32)` | A handle, or `0` |
| `dl_sym` | `(handle i32, name, name_len, value u32)` | `1`, or `0`. Writes a u32 to `value`: a function's table index, or a data symbol's address. |
| `dl_close` | `(handle i32)` | `1` |
| `dl_error` | `(buf, cap u32)` | Length of the message for the last failure |

`dl_open` receives the bytes of the file, so the host needs no path mapping.

### DNS

| Function | Signature | Returns |
| --- | --- | --- |
| `dns_lookup` | `(name, name_len u32, family i32, out, out_cap u32)` | Length of the addresses, one per line. More than `out_cap` asks for a retry with that room. `-1` for an unknown name, `-2` otherwise. |
| `dns_reverse` | `(addr, addr_len, out, out_cap u32)` | Length of the name, or `-1` |

`family` is 4, 6, or 0 for both.

| Function | Signature | Returns |
| --- | --- | --- |
| `dns_query` | `(name, name_len u32, class, type i32, out u32, cap i32)` | The length of the raw answer, cut to `cap`, or `-h_errno` |

`dns_query` is `res_search(3)`: one query to the host's name servers, with recursion desired.
`h_errno` is 1 for an unknown name, 2 to try again, 3 for other errors, and 4 for an answer with no records.

### Pipes and processes

The guest side is `compat/gophper_proc.c`. It implements `pipe`, `posix_spawn`, `waitpid` and `kill`, and `popen` on top of them.

| Function | Signature | Returns |
| --- | --- | --- |
| `pipe_open` | `(read_fd, write_fd i32)` | errno. Both fds then work like stream sockets, with the `sock_*` functions. |
| `proc_spawn` | `(path, path_len u32, search i32, argv, argv_len, envp, envp_len, cwd, cwd_len, fds u32, nfds i32, pid u32)` | errno. Writes the child's pid, an i32, to `pid`. |
| `proc_wait` | `(pid, nohang i32, status u32)` | The pid, `0` when `nohang` and still running, or `-errno`. Writes a wait status, an i32, to `status`. |
| `proc_kill` | `(pid, sig i32)` | errno. Only the instance's own children can be signaled. |

`search` nonzero looks `path` up in the `PATH` of `envp`, as `posix_spawnp` does.
`argv` and `envp` are strings, each ending with a NUL.
A wait status and the signal numbers follow Linux, as wasi-libc's `<sys/wait.h>` does.
A blocking `proc_wait` returns `EINTR` when an interrupt arrives, and `EIO` once the run is over.

`fds` is an array of `nfds` structs, 24 bytes each. Each one is a fd of the child:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `child` i32 | The fd number in the child |
| 4 | `kind` i32 | What the fd is. See the next table. |
| 8 | `value` i32 | See `kind` |
| 12 | `flags` i32 | For a file: 1 read, 2 write, 4 append, 8 create, 16 truncate, 32 exclusive |
| 16 | `path` u32 | For a file: its absolute path |
| 20 | `path_len` u32 | The length of `path` |

| `kind` | The child gets |
| --- | --- |
| 1 | The guest's pipe or socket whose fd is `value` |
| 2 | The instance's stdin, stdout or stderr, for `value` 0, 1 or 2 |
| 3 | The file at `path`, which the host opens again |

### Users, file metadata, locks and the host name

The guest side is `compat/gophper_sys.c`.
Paths are absolute paths inside PHP. The host maps them to its own files.
A path function returns `ENOENT` for a path with no host file behind it, and the guest then keeps what WASI reports.

| Function | Signature | Returns |
| --- | --- | --- |
| `user_ids` | `(ids u32, cap i32)` | The number of groups. Writes the uid, the gid, then up to `cap` group ids, each an i32. |
| `user_get` | `(kind, id i32, name, name_len, out u32, cap i32)` | The length of the entry, `-ENOENT`, or more than `cap` to ask for more room |
| `file_lock` | `(fd i32, path, path_len u32, op i32)` | errno. `flock(2)` on the host file at `path`, for the guest's `fd`. |
| `file_unlock` | `(fd i32)` | errno. Drops the lock of `fd`, which is being closed. |
| `host_name` | `(out u32, cap i32)` | The length of the host name, or `-errno` |
| `path_stat` | `(path, path_len u32, follow i32, out u32)` | errno. Writes the permission bits, the uid and the gid, each a u32. |
| `path_access` | `(path, path_len u32, mode i32)` | errno. `access(2)` on the host file. |
| `path_chmod` | `(path, path_len u32, mode i32)` | errno |
| `path_chown` | `(path, path_len u32, uid, gid, follow i32)` | errno. `-1` keeps the uid or the gid. |

`kind` is 1 for a user by uid, 2 by name, 3 for a group by gid, and 4 by name.
An entry is fields, each ending with a NUL.

| Entry | Fields |
| --- | --- |
| User | Name, password, uid, gid, gecos, home directory, shell |
| Group | Name, password, gid, the members joined by commas |

`op` is `LOCK_SH` 1 or `LOCK_EX` 2, with `LOCK_NB` 4, or `LOCK_UN` 8.
A blocking `file_lock` returns `EINTR` when an interrupt arrives, and `EIO` once the run is over.

### PHP functions written in Go

The guest side is `compat/gophper_fn.c`. `patches/0010` registers the functions after the extensions start.

| Function | Signature | Returns |
| --- | --- | --- |
| `fn_names` | `(out u32, cap i32)` | The length of the names, each ending with a NUL. More than `cap` asks for a retry with that room. |
| `fn_call` | `(name, name_len, args, args_len u32)` | The length of the encoded result, or for an error, `-1` minus the length of its message |
| `fn_take` | `(out u32, cap i32)` | The number of bytes copied. It copies the result or the message of the last `fn_call`. |

`args` is one encoded array: the arguments, with the keys 0 to n-1.
A value is a tag byte and its data. Numbers are little-endian.

| Tag | Value | Data |
| --- | --- | --- |
| `N` | null | None |
| `F`, `T` | false, true | None |
| `i` | int | An i64 |
| `d` | float | An f64 |
| `s` | string | A u32 length, then the bytes |
| `a` | array | A u32 count, then a key (`i` or `s`) and a value for each entry |

## Exports

### Interrupt flags

| Global | Holds the address of |
| --- | --- |
| `gophper_vm_interrupt` | A pointer to `EG(vm_interrupt)`, one byte |
| `gophper_timed_out` | A pointer to `EG(timed_out)`, one byte |
| `gophper_hard_timeout` | A pointer to `EG(hard_timeout)`, an i32 |

The host must not run `_start` before it reads these. Instantiate without start functions, then call `_start`.

### Symbols for side modules

The binaries are linked with `--export-all`, `--export-table`, `--growable-table` and `--export=__stack_pointer`.
All of libc, zlib, OpenSSL and the C++ runtime (libc++, libc++abi and libunwind, with exceptions) are linked in. So every function and data symbol of theirs is exported, as are:

| Export | Kind |
| --- | --- |
| `memory` | The linear memory |
| `__indirect_function_table` | The function table, growable |
| `__stack_pointer` | A mutable i32 global |
| `__c_longjmp` | The tag `setjmp`/`longjmp` throw |
| `__cpp_exception` | The tag C++ exceptions throw |

A side module calls `read`, `poll` and the rest through their `__wrap_` exports, so that socket fds work.
A side module may import a function it defines itself, weakly. The host then resolves the import to the module's own export.
`calloc` gives a side module its memory.

## Extensions

An extension is a side module, built by `scripts/build-ext.sh`.
It follows the WebAssembly dynamic linking convention (the `dylink.0` section).

| Requirement | Value |
| --- | --- |
| Compiled with | `-fPIC -fvisibility=default`, against the configured php-src |
| Linked with | `-shared -Wl,--experimental-pic -Wl,--unresolved-symbols=import-dynamic` |
| Imports | `env`, `GOT.mem` and `GOT.func`, resolved by the host against the exports above |
| Runtime features | Exception handling and extended constant expressions |

## Environment and files

| What | Meaning |
| --- | --- |
| `GOPHPER_CWD` | The working directory to start in. Unset before `main()`. |
| `/.gophper/socket` | An empty file the host mounts. Opening it reserves a socket fd. |
| `/etc/gophper/php.ini` | The default php.ini (`--with-config-file-path`). |
| `/usr/local/lib/php/extensions` | The default `extension_dir`. Extensions are `<name>.so` files there. |
| `/etc/gophper/ssl` | OPENSSLDIR: `openssl.cnf` and the CA bundle `cert.pem`. The host mounts `phpwasm.SSL` there, read-only. |

## Errno

Errors use WASI numbering (`__errno_values.h` in wasi-libc), such as `EAGAIN` 6, `EINTR` 27 and `ECONNREFUSED` 14.
