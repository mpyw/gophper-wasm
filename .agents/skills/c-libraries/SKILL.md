---
name: c-libraries
description: Add a C library, a built-in extension, or a loadable extension (ext/*.so) to gophper-wasm, or change how one is built. Use before editing scripts/deps.sh, scripts/build-ext.sh, SHARED_EXTENSIONS, licenses/, or ICU, OpenSSL, curl, libpq and the other libraries' flags.
---

# C libraries and extensions

| Kind | Linked into | Built by |
| --- | --- | --- |
| A library for php.wasm, such as OpenSSL | `php.wasm` and `php-cgi.wasm` | `scripts/deps.sh`, then `scripts/build.sh` |
| A built-in extension, such as curl | Both binaries | A `--with-` flag in `scripts/build.sh` |
| A loadable extension, such as intl | `ext/NAME.so.gz`, a wasm side module | `scripts/build-ext.sh`, with its library from `scripts/deps.sh` as PIC |

## Add a library

1. Pin its version in `scripts/env.sh`.
2. Write `build_NAME` in `scripts/deps.sh`, as the others are: `done_stamp` first, `fetch`, a static cross build with `DEP_CFLAGS`, then `mark_done`. Add NAME to the default list at the bottom.
3. A library for an extension builds with `-fPIC -DNDEBUG`. php.wasm does not export `__assert_fail`.
4. Check it does not use thread-local storage: `llvm-objdump -d -r lib.a | grep __tls_base` prints nothing.
5. Add its license to `scripts/licenses.sh`, run it, and add a row to README's license table.
6. Add its version to README's "Versions inside" table. No other place in either README names a version.

## Add a loadable extension

1. Add it to `SHARED_EXTENSIONS` in `scripts/env.sh`.
2. Add a `case` in `scripts/build-ext.sh` for its `EXT_CFLAGS`, `EXT_LIBS`, and `EXT_FILES` (files it reads at runtime, shipped as `ext/NAME-FILE.gz`). A PECL extension sets `SRC` to its source under `.build/deps-src`, fetched by `scripts/deps.sh`.
3. Run `scripts/build-ext.sh NAME`. gophper's `extension_test.go` loads every extension in `ext/`. Add a check there for what the extension does.
4. Add it to README's "These come built" table, with no version, and to gophper's README extension table.

## Rejected designs

| Idea | Why not |
| --- | --- |
| curl as a loadable extension | Guzzle and Composer use it all the time, and OpenSSL, zlib and the sockets are in php.wasm anyway. It is built in, for about 250 KB. |
| libc++ inside a C++ extension | wasi-sdk's libc++ is not PIC, so it cannot go in a side module. php.wasm links the exception-enabled libc++, libc++abi and libunwind whole, and intl.so imports them. |
| ICU's data inside intl.so | Static data is part of every instance's memory, 33 MB each, even when no script uses intl. The archive file is read only when needed. |
| Passing ICU's `-D` settings through `CPPFLAGS` | The quotes of `U_ICU_DATA_DEFAULT_DIR` did not survive configure and make, and ICU found no data. A force-included header carries them. |
| ICU reading its data with mmap | It asks for `MAP_SHARED`, which wasi's emulated mmap lacks. `U_HAVE_MMAP=0` reads the file instead. |
| phpredis without its session handler | `session.save_handler=redis` in php.ini is common outside frameworks. `scripts/ext/phpredis-session.patch` follows PHP 8.6's `zend_string` save path, and `scripts/ext/phpredis.h` maps the other removed APIs. |
| ICU's full data | 33 MB. `scripts/icu-filter.json` keeps English and Japanese, 13 MB. ICU's host build makes the data from its sources. |
| Committing the binaries without `licenses/` | Whoever distributes them must pass on the licenses. Run `scripts/licenses.sh` after a version in `env.sh` changes, and update README's table. |
| Assertions in an extension's library | php.wasm did not export `__assert_fail`. The libraries for extensions build with `-DNDEBUG`. |
| Running a library's configure with compat force-included | configure declares each function it checks, such as `char connect();`, which conflicts. `scripts/deps.sh` links its tests against compat objects (`compat_link`) and force-includes the header only in `make`. |
| A second, PIC build of zlib and OpenSSL for extensions | php.wasm exports them, and a side module resolves them there. Only the extension's own library is built as PIC. |
| `_GNU_SOURCE` for OpenSSL | OpenSSL then expects the GNU `strerror_r`, and wasi-libc (musl) has the POSIX one. `scripts/deps.sh` drops it for OpenSSL only. |
| OpenSSL with QUIC | PHP does not use QUIC, and `ssl/rio` would need `socketpair` from the host. |
| zlib's own `make` on a Mac | Its configure picks macOS `libtool`, which writes an empty archive from wasm objects. `scripts/deps.sh` passes `llvm-ar`. |
| Overriding configure's `EXTRA_LIBS` in `make` | It drops `-lxml2 -lssl` and the rest. `scripts/build.sh` appends to it. |
| No `openssl.cnf` | `openssl_pkey_new()` fails without one. `ssl/` ships it and the CA bundle, mounted at OPENSSLDIR. |
| libpq with configure's `PTHREAD_CFLAGS` (`-pthread`) | It enables wasm atomics, so libpq reads `errno` as thread-local, through `__tls_base`. wasi-libc's `errno` is a plain global. `PQconnectdb` crashed on its first `errno = 0`, with an out-of-bounds access. `scripts/deps.sh` empties `PTHREAD_CFLAGS`. Check a new library with `llvm-objdump -d -r lib.a \| grep __tls_base`. |
