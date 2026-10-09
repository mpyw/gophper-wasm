<div align="center">

<img src=".github/assets/icon.png" width="200" alt="A Go gopher and a purple elephant in mechanical sunglasses, leaping out of a hexagon">

# gophper-wasm

PHP, compiled to WebAssembly, packaged as a Go module.

[![Build](https://github.com/mpyw/gophper-wasm/actions/workflows/build.yml/badge.svg)](https://github.com/mpyw/gophper-wasm/actions/workflows/build.yml)
[![Go Reference](https://pkg.go.dev/badge/github.com/mpyw/gophper-wasm.svg)](https://pkg.go.dev/github.com/mpyw/gophper-wasm)

</div>

[gophper](https://github.com/mpyw/gophper) runs these binaries on [wazero](https://github.com/tetratelabs/wazero).
That is how gophper runs PHP from Go with no cgo.

> [!WARNING]
> This is an experiment. The ABI and the Go API will change, and PHP may be a release candidate.

> [!TIP]
> **To run PHP, use gophper, not this module.**
> This module only holds the binaries.
> You need it directly only to write your own host, or to rebuild PHP.

> [!CAUTION]
> **main and the other branches hold no binaries.** Only release tags, such as `v0.0.0`, carry them.
> Depend on a tag: `go get github.com/mpyw/gophper-wasm@latest`, or a version such as `@v0.0.0`.
> `@main` or a commit hash gets the sources alone, and the package does not build.
> To work from a branch, build the binaries first. See [Rebuild the binaries](#rebuild-the-binaries).

## In one table

| Question | Answer |
| --- | --- |
| What is inside? | The `php` CLI and `php-cgi`, as `wasm32-wasip1` modules. Built from php-src with a few small patches. |
| Do I need a C compiler to use it? | No. Each release tag carries the binaries, gzipped. `go get` is enough. |
| Does it run on any WASI runtime? | No. It imports host functions from a module named `gophper`. [ABI.md](ABI.md) lists them. |
| Which PHP version? | See [Versions inside](#versions-inside), or the constant `PHPVersion`. |
| Which extensions? | See [What is built in](#what-is-built-in). PDO with MySQL, PostgreSQL and SQLite is included. |

## Use it from Go

```sh
go get github.com/mpyw/gophper-wasm
```

```go
import phpwasm "github.com/mpyw/gophper-wasm"

cli, err := phpwasm.CLI() // the php CLI, decompressed
cgi, err := phpwasm.CGI() // php-cgi, decompressed
```

| Name | Kind | What it is |
| --- | --- | --- |
| `CLI()` | function | The `php` CLI SAPI, as wasm bytes |
| `CGI()` | function | The `php-cgi` SAPI, as wasm bytes. It runs one request in plain CGI mode. With `-b SOCKET`, it serves FastCGI requests on a Unix socket, one at a time. |
| `SSL` | `fs.FS` | `openssl.cnf` and Mozilla's CA bundle. A host mounts it at `/etc/gophper/ssl`. |
| `PHPVersion` | constant | The php-src release the binaries were built from |
| `ABIVersion` | constant | The version of the contract with the host |
| `ext.Names()` | function | The loadable extensions in package `ext` |
| `ext.Open(name)` | function | One extension's `.so`, decompressed |

> [!IMPORTANT]
> A host must compare `ABIVersion` with the version it implements.
> If they differ, it must refuse to run the binaries.
> gophper does this.

## What is built in

Both binaries have the same extensions.

| Area | Extensions |
| --- | --- |
| Core | Core, date, hash, json, pcre, random, Reflection, SPL, standard, Zend OPcache |
| Text | ctype, iconv, mbstring (no `mb_ereg*`), tokenizer |
| Data | bcmath, calendar, exif, fileinfo, filter, lexbor, uri, zlib |
| XML | dom, libxml, SimpleXML, xml, xmlreader, xmlwriter |
| Crypto | openssl |
| Database | PDO, pdo_mysql, pdo_pgsql, pdo_sqlite, mysqli, mysqlnd, pgsql, sqlite3 |
| Network | curl |
| Other | pcntl (no `fork`), Phar, posix, session |

These C libraries are linked in:

| Library | Built without |
| --- | --- |
| zlib | |
| libxml2 | Threads, modules, HTTP, ICU |
| SQLite | WAL. Locking uses dot files, SQLite's own WASI mode. |
| OpenSSL | Assembly, threads, QUIC, dynamic loading |
| libpq | Threads, GSSAPI, LDAP, zlib, ICU |
| libcurl | HTTP/2, IDN, LDAP, SSH, and protocols other than HTTP, FTP, file and WebSocket |

All of wasi-libc, zlib, OpenSSL and the C++ runtime (libc++ with exceptions) are linked in whole, so that an extension finds any of their functions.

Other extensions are loaded at runtime, with `extension=` or `dl()`.
Each one is a wasm file, so one file works on every platform.
These come built:

| Extension | Library inside |
| --- | --- |
| sodium | libsodium |
| zip | libzip. zlib and OpenSSL come from the binaries. |
| gd | php-src's bundled libgd, with libpng and libjpeg-turbo |
| intl | ICU, with English and Japanese: `scripts/icu-filter.json`. ICU's data, 13 MB, ships as a file beside `intl.so`: `ext.Files("intl")`. |
| bz2 | bzip2 |
| gmp | GMP, under the LGPL. See [License](#license). |
| redis | phpredis, with `session.save_handler=redis` and the JSON serializer |
| dl_test | None. php-src's extension for testing `dl()`. |

You can also build your own with `scripts/build-ext.sh`.
[ABI.md](ABI.md#extensions) lists what an extension must be.

> [!IMPORTANT]
> An extension is a wasm side module, not a native `.so`.
> Build it against the same php-src version and configuration as the binaries.
> PHP's internal API differs between builds, as with native extensions.

### Versions inside

Every version is pinned in `scripts/env.sh`. This table follows it.

| Component | Version | In |
| --- | --- | --- |
| PHP (php-src) | 8.6.0RC3 | Both binaries |
| zlib | 1.3.2 | Both binaries |
| libxml2 | 2.15.4 | Both binaries |
| SQLite | 3.53.4 | Both binaries |
| OpenSSL | 4.0.3 | Both binaries |
| libpq | PostgreSQL 18.6 | Both binaries |
| libcurl | 8.22.0 | Both binaries |
| libsodium | 1.0.22 | `sodium.so` |
| libzip | 1.11.4 | `zip.so` |
| libpng | 1.6.58 | `gd.so` |
| libjpeg-turbo | 3.2.0 | `gd.so` |
| ICU | 78.3 | `intl.so` |
| bzip2 | 1.0.8 | `bz2.so` |
| GMP | 6.3.0 | `gmp.so` |
| phpredis | 6.3.0 | `redis.so` |
| Mozilla's CA certificates | 2026-09-25 | `ssl/cert.pem` |
| wasi-sdk | 34.0 | The toolchain |
| binaryen | 133 | The toolchain |

## How it differs from native PHP

| Area | Behavior |
| --- | --- |
| `PHP_OS` | `WASI` |
| Fibers | `Fiber::start()` throws. WASI cannot switch stacks. |
| Processes | `proc_open()`, `exec()`, `popen()` and the rest work. The host starts the child with Go's `os/exec`. |
| Users, groups, permissions and locks | The host's. `posix_getuid()`, `fileperms()`, `chmod()` and `flock()` ask the host. |
| Signals | The host passes them in. `pcntl_signal()` handlers, `pcntl_alarm()` and masks work. `max_execution_time` uses the host's timer. |
| Sockets and DNS | Work. The host does them, through Go's `net` package. |
| `random_bytes()` | Reads WASI's `random_get`, not `/dev/urandom` |
| php.ini | Read from `/etc/gophper`. The host provides that directory. |
| Extension directory | `/usr/local/lib/php/extensions` |

## Releases

The module's version is not PHP's version. Read `PHPVersion` for that.

| Change | Version bump |
| --- | --- |
| `ABIVersion` changes | Minor, while below v1 |
| Anything else, such as a new php-src release | Patch |

To release, run the `build` workflow on main by hand, with the version, such as `v0.0.0`.
It builds and tests from scratch.
Then it commits the sources and the binaries as an orphan commit, and pushes only its tag.

| Ref | Holds |
| --- | --- |
| `main` and other branches | The sources |
| A tag, such as `v0.0.0` | The sources and the binaries, in a commit of its own. Its message names the source commit. |

> [!NOTE]
> A release commit has no parent, and no branch contains it.
> A clone of main with `git clone --single-branch --no-tags` downloads no binaries.

## Rebuild the binaries

CI builds the binaries from scratch on every push, and tests them.
Build locally to try a change before you push it.
The build runs on macOS and Linux.

### Steps

```sh
brew install bison re2c autoconf pkgconf   # macOS
sudo apt install bison re2c autoconf pkgconf   # Debian, Ubuntu
scripts/fetch.sh             # 1. download wasi-sdk, binaryen and php-src, apply patches/
scripts/deps.sh              # 2. build the C libraries
scripts/build.sh             # 3. build PHP, write *.wasm.gz and version.go
scripts/build-ext.sh         # 4. build the loadable extensions in ext/
```

The outputs, `php-src/`, `.build/` and `.tools/` are ignored by Git.
Branches hold only the sources, so the Go package builds only after these steps.

| Script | What it does | Writes |
| --- | --- | --- |
| `scripts/env.sh` | Pins every version. The other scripts read it. | Nothing |
| `scripts/fetch.sh` | Downloads wasi-sdk and binaryen, and clones php-src. `--force` clones again. | `.tools/`, `php-src/` |
| `scripts/deps.sh [lib...]` | Builds the libraries. A library already built is skipped. | `.build/deps/` |
| `scripts/build.sh` | Configures and builds PHP. `--reconfigure` runs configure again. | `php.wasm.gz`, `php-cgi.wasm.gz`, `version.go` |
| `scripts/build-ext.sh [NAME...]` | Builds php-src extensions as loadable modules. Default: `SHARED_EXTENSIONS` in `env.sh` | `ext/NAME.so.gz` |
| `scripts/export-patches.sh` | Saves your changes in `php-src/` | `patches/` |
| `scripts/licenses.sh` | Collects the licenses of every component, after `scripts/deps.sh` | `licenses/` |

### Changing php-src

1. Edit the files in `php-src/`.
2. Put a comment with `GOPHPER:` next to each change.
3. Run `scripts/export-patches.sh`.

> [!WARNING]
> `scripts/export-patches.sh` refuses a change without a `GOPHPER:` marker.
> The marker lets you find every change with `grep GOPHPER:`.

## How the build works

php-src is built with [wasi-sdk](https://github.com/WebAssembly/wasi-sdk).
`setjmp` and `longjmp` use WebAssembly exception handling, in the standard (exnref) encoding.
Two directories hold every difference from php-src.

### patches/

| Patch | What it changes |
| --- | --- |
| `0001-configure-wasi.patch` | configure builds without fibers, and reports `PHP_OS` as `WASI` |
| `0002-zend-fibers-unsupported-on-wasi.patch` | `Fiber::start()` throws instead of switching stacks |
| `0003-zend-alloc-aligned-alloc-on-wasi.patch` | Zend's allocator uses `aligned_alloc()`. WASI's emulated `mmap` cannot align or partially unmap. |
| `0004-zend-timeout-through-go-host.patch` | The execution timer goes to the host. WASI has no signals. |
| `0005-random-from-wasi-random-get.patch` | `random_bytes()` uses WASI's `random_get` |
| `0006-getrusage-fields-on-wasi.patch` | `getrusage()` reports only the CPU times, as WASI's `struct rusage` has no more |
| `0007-standard-functions-on-wasi.patch` | configure enables `fnmatch()` and the DNS functions, which wasi-libc or compat/ have |
| `0008-zend-signals-through-go-host.patch` | Zend's interrupt check takes the signals the host marked pending |
| `0009-opcache-file-cache-attributed-const-on-32-bit.patch` | Fixes opcache's file cache for attributes on constants, which kept a raw pointer on 32-bit targets |
| `0010-functions-from-go-host.patch` | Registers the PHP functions the host writes in Go, after the extensions start |
| `0011-opcache-lock-file-on-wasi.patch` | opcache's lock file takes no record locks. WASI has none, and the shared memory never leaves the instance. |
| `0012-openssl-listen-failure.patch` | Fixes `ssl://` and `tls://` servers, which took a failed `listen()` for success |
| `0013-listen-error-text.patch` | A failed `listen()` says why. The host binds a TCP socket only when it listens. |
| `0014-yield-to-the-go-scheduler.patch` | The interrupt check calls the host's `yield` every 65536 times. Go cannot preempt WebAssembly. |
| `0015-fastcgi-shutdown-writes.patch` | php-cgi shuts the writing side with `SHUT_WR`, not with 1, which is `SHUT_RD` in WASI |

### compat/

C code for what wasi-libc lacks. It is linked into both binaries.

| File | What it provides |
| --- | --- |
| `gophper_compat.c`, `include/` | Small POSIX pieces: `mkstemp`, `tmpfile`, `syslog` to stderr, uid 0. `fork` fails. `tzset` does nothing, since PHP has its own time zones. |
| `include/sys/socket.h` | Declares `socket()` and the rest, which wasi-libc hides, so that a library's configure finds them |
| `gophper_net.c` | Sockets, `socketpair` and DNS through the host. It takes over `read`, `poll` and the rest for socket and pipe fds. |
| `gophper_fd.c` | `dup`, `dup2` and `F_DUPFD`. WASI cannot duplicate an fd, so a duplicate is an alias. It also remembers the path of each open file, for child processes. |
| `gophper_dns.c`, `include/resolv.h` | `res_search` through the host, and `dn_expand`, for `dns_get_record()` |
| `gophper_sys.c`, `include/sys/resource.h` | Users and groups, file permissions and owners, `flock`, the host name, and resource limits, through the host |
| `gophper_signal.c` | Signal handlers, masks, `sigwaitinfo` and `alarm`. The host delivers signals by interrupting the VM. |
| `gophper_proc.c`, `include/spawn.h` | `posix_spawn`, `waitpid`, `kill`, `pipe` and `popen`, through the host |
| `gophper_dl.c` | `dlopen` and friends, through the host's dynamic linker |
| `gophper_fn.c` | PHP functions that call Go functions on the host |
| `gophper_zend.c` | Exports the addresses of the interrupt flags. The host writes them on a timeout. |

## License

This repository's own code (`compat/`, `scripts/` and the Go files) is under the [MIT License](LICENSE).
The binaries and extensions contain other software, under their own licenses.
Their texts are in [licenses/](licenses), and in Go as `phpwasm.Licenses`.

The mascot shows a Go gopher riding an elephant in the style of PHP's ElePHPant.
The Go gopher was designed by Renée French, under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The ElePHPant was designed by Vincent Pontier.

| Component | In | License | Text |
| --- | --- | --- | --- |
| PHP | Both binaries | BSD 3-Clause | `php.txt` |
| zlib | Both binaries | zlib | `zlib.txt` |
| libxml2 | Both binaries | MIT | `libxml2.txt` |
| SQLite | Both binaries | Public domain | `sqlite.txt` |
| OpenSSL | Both binaries | Apache 2.0 | `openssl.txt` |
| libpq (PostgreSQL) | Both binaries | PostgreSQL | `postgresql.txt` |
| libcurl | Both binaries | curl (MIT-style) | `curl.txt` |
| wasi-libc | Both binaries | MIT, Apache 2.0 | `wasi-libc.txt`, `wasi-libc-mit.txt` |
| libc++, libc++abi, libunwind | Both binaries | Apache 2.0 with LLVM exceptions | `llvm.txt` |
| Mozilla's CA certificates | `ssl/cert.pem` | MPL 2.0 | `mpl-2.0.txt` |
| libsodium | `sodium.so` | ISC | `libsodium.txt` |
| libzip | `zip.so` | BSD 3-Clause | `libzip.txt` |
| libgd | `gd.so` | gd (BSD-style) | `libgd.txt` |
| libpng | `gd.so` | libpng | `libpng.txt` |
| libjpeg-turbo | `gd.so` | IJG, BSD 3-Clause, zlib | `libjpeg-turbo.txt` |
| ICU | `intl.so` and its data | Unicode | `icu.txt` |
| bzip2 | `bz2.so` | bzip2 (BSD-style) | `bzip2.txt` |
| GMP | `gmp.so` | LGPL 3.0 | `gmp-lgpl-3.0.txt`, `gmp-gpl-3.0.txt` |
| phpredis | `redis.so` | PHP License 3.01 | `phpredis.txt` |

gd.so is based in part on the work of the Independent JPEG Group.

> [!IMPORTANT]
> `gmp.so` links GMP statically, under the LGPL.
> Whoever distributes it must let its users link it with another GMP.
> This repository is that means: its scripts build `gmp.so` from GMP's sources.
> `scripts/env.sh` pins GMP's version, and `scripts/deps.sh` downloads its source from https://ftp.gnu.org/gnu/gmp/.
> The other binaries and extensions do not contain GMP.
