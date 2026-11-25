---
name: dependency-upgrade
description: Update a C library, the toolchain, or the CA bundle that gophper-wasm builds with, such as OpenSSL, curl, libxml2, SQLite, ICU, wasi-sdk or binaryen. Use when bumping any version in scripts/env.sh other than PHP_TAG, or after a security advisory for one of them.
---

# Upgrade a dependency

Every version is pinned in `scripts/env.sh`. `scripts/deps.sh` rebuilds a library when its version changes: each build leaves a stamp named by the version and the compat headers' hash.

## Where to find new versions

| Library | Variable | Releases |
| --- | --- | --- |
| zlib | `ZLIB_VERSION` | https://github.com/madler/zlib/releases |
| libxml2 | `LIBXML2_VERSION` | https://download.gnome.org/sources/libxml2/ |
| SQLite | `SQLITE_VERSION`, `SQLITE_YEAR` | https://www.sqlite.org/download.html. The number is `3XXYYZZ00`, and the year is in its download path. |
| OpenSSL | `OPENSSL_VERSION` | https://github.com/openssl/openssl/releases |
| libpq | `POSTGRES_VERSION` | https://www.postgresql.org/ftp/source/ |
| libsodium | `SODIUM_VERSION` | https://download.libsodium.org/libsodium/releases/ |
| libzip | `LIBZIP_VERSION` | https://libzip.org/download/ |
| curl | `CURL_VERSION` | https://curl.se/download.html |
| libpng | `LIBPNG_VERSION` | https://download.sourceforge.net/libpng/ |
| libjpeg-turbo | `LIBJPEG_TURBO_VERSION` | https://github.com/libjpeg-turbo/libjpeg-turbo/releases |
| ICU | `ICU_VERSION` | https://github.com/unicode-org/icu/releases |
| bzip2 | `BZIP2_VERSION` | https://sourceware.org/pub/bzip2/ |
| GMP | `GMP_VERSION` | https://gmplib.org/ |
| phpredis | `PHPREDIS_VERSION` | https://github.com/phpredis/phpredis/tags |
| wasi-sdk | `WASI_SDK_VERSION` | https://github.com/WebAssembly/wasi-sdk/releases |
| binaryen | `BINARYEN_VERSION` | https://github.com/WebAssembly/binaryen/releases |
| CA bundle | `ssl/cert.pem` | https://curl.se/docs/caextract.html. Download `cacert.pem` over it. |

## Steps

1. Change the version in `scripts/env.sh`.
2. Build and test:

   ```sh
   scripts/fetch.sh && scripts/deps.sh LIB && scripts/build.sh && scripts/build-ext.sh && go test ./...
   ```

   For wasi-sdk or binaryen, run `scripts/fetch.sh` first, then `scripts/deps.sh` with no arguments, and `scripts/build.sh --reconfigure`. Both shape every object.
3. Check the library's own changes:

   | Library | What breaks |
   | --- | --- |
   | Any | A new `-pthread` or thread-local use. Check with `llvm-objdump -d -r lib.a \| grep __tls_base`. It must print nothing. |
   | ICU | The major version names the data file, `icudt<major>l.dat`, which ships beside intl.so. `scripts/icu-filter.json` keeps English and Japanese. |
   | OpenSSL | New assembly or QUIC defaults. `scripts/deps.sh` turns both off. |
   | wasi-sdk | The EH encoding (`-mllvm -wasm-use-legacy-eh=false` must stay) and wasi-libc's prototypes, which compat wraps |
   | binaryen | wasm-opt's validation of the linked module |

4. Run `scripts/licenses.sh`. A license that changed shows in `git diff licenses/`.
5. Update README's "Versions inside" table. It is the only place that names versions. Check its license table too.
6. Run gophper's tests against this checkout, then release as a patch version (release skill).
