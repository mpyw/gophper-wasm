#!/usr/bin/env bash
# Collect the licenses of everything in the binaries into licenses/.
# Run it after scripts/deps.sh, when a version in env.sh changes.
set -euo pipefail

. "$(dirname "$0")/env.sh"
SRC="$BUILD/deps-src"
OUT="$ROOT/licenses"
rm -rf "$OUT"
mkdir -p "$OUT"

copy() { # name file
  cp "$2" "$OUT/$1.txt"
}
fetch() { # name url
  curl -fsSL -o "$OUT/$1.txt" "$2"
}

copy php "$PHP_SRC/LICENSE"
copy zlib "$SRC/zlib-$ZLIB_VERSION/LICENSE"
copy libxml2 "$SRC/libxml2-$LIBXML2_VERSION/Copyright"
copy openssl "$SRC/openssl-$OPENSSL_VERSION/LICENSE.txt"
copy postgresql "$SRC/postgresql-$POSTGRES_VERSION/COPYRIGHT"
copy curl "$SRC/curl-$CURL_VERSION/COPYING"
copy libsodium "$SRC/libsodium-$SODIUM_VERSION/LICENSE"
copy libzip "$SRC/libzip-$LIBZIP_VERSION/LICENSE"
copy libpng "$SRC/libpng-$LIBPNG_VERSION/LICENSE"
copy libjpeg-turbo "$SRC/libjpeg-turbo-$LIBJPEG_TURBO_VERSION/LICENSE.md"
copy icu "$SRC/icu-$ICU_VERSION/icu/LICENSE"
copy bzip2 "$SRC/bzip2-$BZIP2_VERSION/LICENSE"
copy gmp-lgpl-3.0 "$SRC/gmp-$GMP_VERSION/COPYING.LESSERv3"
copy gmp-gpl-3.0 "$SRC/gmp-$GMP_VERSION/COPYINGv3"
copy phpredis "$SRC/phpredis-$PHPREDIS_VERSION/LICENSE"
# SQLite is in the public domain, and says so in each source file.
printf '%s\n' "SQLite is in the public domain. See https://sqlite.org/copyright.html" > "$OUT/sqlite.txt"

# Not in any source tarball here.
fetch libgd "https://raw.githubusercontent.com/libgd/libgd/master/COPYING"
fetch wasi-libc "https://raw.githubusercontent.com/WebAssembly/wasi-libc/main/LICENSE"
fetch wasi-libc-mit "https://raw.githubusercontent.com/WebAssembly/wasi-libc/main/LICENSE-MIT"
fetch llvm "https://raw.githubusercontent.com/llvm/llvm-project/main/libcxx/LICENSE.TXT"
fetch mpl-2.0 "https://www.mozilla.org/media/MPL/2.0/index.txt"

ls "$OUT"
