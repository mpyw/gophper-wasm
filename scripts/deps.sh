#!/usr/bin/env bash
# Build the C libraries that php.wasm links statically, into .build/deps.
# Usage: scripts/deps.sh [library...]   (default: all; a built library is skipped)
set -euo pipefail

. "$(dirname "$0")/env.sh"
COMPAT="$ROOT/compat"
SRC="$BUILD/deps-src"
mkdir -p "$DEPS/lib/pkgconfig" "$DEPS/include" "$SRC"

# The libraries see the compat declarations too (sockets, users, ...).
DEP_CFLAGS="$CFLAGS -D_GNU_SOURCE -I$COMPAT/include -include $COMPAT/gophper_compat.h"

# A library is built again when the compat headers change: they can change
# types, such as sigset_t, that the library and PHP must agree on.
HEADERS_HASH="$( (cat "$COMPAT/gophper_compat.h"; find "$COMPAT/include" -type f -print0 | sort -z | xargs -0 cat) | shasum | cut -c1-12)"

# fetch unpacks a fresh copy, so that no object from an older build stays.
fetch() { # url dir
  local url="$1" dir="$2"
  rm -rf "${SRC:?}/$dir"
  echo "fetching $url"
  curl -fsSL "$url" | tar xz -C "$SRC" 2>/dev/null || curl -fsSL "$url" | tar xJ -C "$SRC"
  cd "$SRC/$dir"
}

done_stamp() { [[ -f "$DEPS/.$1-$2-$HEADERS_HASH" ]]; }
mark_done() { rm -f "$DEPS/.$1-"*; touch "$DEPS/.$1-$2-$HEADERS_HASH"; }

build_zlib() {
  done_stamp zlib "$ZLIB_VERSION" && return
  fetch "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" "zlib-$ZLIB_VERSION"
  CFLAGS="$DEP_CFLAGS" ./configure --static --prefix="$DEPS"
  # configure picks macOS libtool on a Mac, which cannot read wasm objects.
  make -j"$NPROC" libz.a AR="$AR" ARFLAGS=rcs
  make install AR="$AR" ARFLAGS=rcs
  mark_done zlib "$ZLIB_VERSION"
}

# Autotools projects: cross compile for wasm32-wasi, static only.
autotools() {
  CFLAGS="$DEP_CFLAGS" PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH="" \
    ./configure --host=wasm32-wasi --build="$BUILD_TRIPLE" --prefix="$DEPS" \
    --enable-static --disable-shared "$@"
  make -j"$NPROC"
  make install
}

build_libxml2() {
  done_stamp libxml2 "$LIBXML2_VERSION" && return
  local minor="${LIBXML2_VERSION%.*}"
  fetch "https://download.gnome.org/sources/libxml2/$minor/libxml2-$LIBXML2_VERSION.tar.xz" "libxml2-$LIBXML2_VERSION"
  # No threads or modules in wasm; zlib for compressed documents.
  autotools --without-python --without-threads --without-modules --without-lzma \
    --without-http --without-ftp --without-icu --with-zlib="$DEPS" --with-iconv
  mark_done libxml2 "$LIBXML2_VERSION"
}

build_sqlite() {
  done_stamp sqlite "$SQLITE_VERSION" && return
  fetch "https://www.sqlite.org/$SQLITE_YEAR/sqlite-autoconf-$SQLITE_VERSION.tar.gz" "sqlite-autoconf-$SQLITE_VERSION"
  # The amalgamation needs no configure. SQLite knows WASI (SQLITE_WASI):
  # dotfile locking, no WAL, no loadable extensions, no threads.
  $CC $DEP_CFLAGS -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_COLUMN_METADATA \
    -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_ENABLE_DBSTAT_VTAB -c sqlite3.c -o sqlite3.o
  "$AR" rcs "$DEPS/lib/libsqlite3.a" sqlite3.o
  cp sqlite3.h sqlite3ext.h "$DEPS/include/"
  local version
  version="$(sed -nE 's/^#define SQLITE_VERSION +"([^"]+)".*/\1/p' sqlite3.h)"
  cat > "$DEPS/lib/pkgconfig/sqlite3.pc" <<PC
prefix=$DEPS
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine
Version: $version
Libs: -L\${libdir} -lsqlite3
Cflags: -I\${includedir}
PC
  mark_done sqlite "$SQLITE_VERSION"
}

build_openssl() {
  done_stamp openssl "$OPENSSL_VERSION" && return
  fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" "openssl-$OPENSSL_VERSION"
  # No WASI target in OpenSSL: a generic 32-bit platform with no assembly,
  # threads or dynamic loading. Sockets come from compat/ through the host.
  # The CA bundle path inside PHP is /etc/gophper/ssl.
  # Without _GNU_SOURCE: OpenSSL then expects the GNU strerror_r, and
  # wasi-libc (musl) has the POSIX one.
  CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="${DEP_CFLAGS/-D_GNU_SOURCE /}" ./Configure linux-generic32 \
    no-asm no-threads no-shared no-dso no-tests no-docs no-ui-console no-afalgeng no-async \
    no-secure-memory no-apps no-quic -static --prefix="$DEPS" --libdir=lib --openssldir=/etc/gophper/ssl
  make -j"$NPROC" build_libs
  make install_dev
  mark_done openssl "$OPENSSL_VERSION"
}

build_libpq() {
  done_stamp libpq "$POSTGRES_VERSION" && return
  fetch "https://ftp.postgresql.org/pub/source/v$POSTGRES_VERSION/postgresql-$POSTGRES_VERSION.tar.gz" "postgresql-$POSTGRES_VERSION"
  # Only the client library, with TLS from our OpenSSL.
  CFLAGS="$DEP_CFLAGS" LDFLAGS="-L$DEPS/lib" CPPFLAGS="-I$DEPS/include" \
    PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH="" \
    ./configure --host=wasm32-wasi --build="$BUILD_TRIPLE" --prefix="$DEPS" \
    --with-template=linux --without-readline --without-icu --without-zlib --without-gssapi --without-ldap --with-ssl=openssl
  # Without -pthread: it enables wasm atomics, so libpq would read errno as
  # thread-local. wasi-libc's errno is a plain global, so the first
  # errno = 0 writes out of bounds.
  local nothread=(PTHREAD_CFLAGS= PTHREAD_LIBS=)
  make -C src/include
  make -C src/common -j"$NPROC" "${nothread[@]}"
  make -C src/port -j"$NPROC" "${nothread[@]}"
  make -C src/interfaces/libpq -j"$NPROC" "${nothread[@]}" all-static-lib
  make -C src/interfaces/libpq install-lib-static install-lib-pc
  # The client headers only. src/include's install would add the server's.
  cp src/interfaces/libpq/libpq-fe.h src/interfaces/libpq/libpq-events.h \
    src/include/postgres_ext.h "$DEPS/include/"
  mkdir -p "$DEPS/include/libpq"
  cp src/include/libpq/libpq-fs.h "$DEPS/include/libpq/"
  # libpq.a needs these two from the same build, and OpenSSL. PHP's configure
  # reads Libs without --static, so they go there.
  cp src/common/libpgcommon_shlib.a src/port/libpgport_shlib.a "$DEPS/lib/"
  sed -E 's|^Libs: (.*)$|Libs: \1 -lpgcommon_shlib -lpgport_shlib -lssl -lcrypto|' "$DEPS/lib/pkgconfig/libpq.pc" > "$DEPS/lib/pkgconfig/libpq.pc.new"
  mv "$DEPS/lib/pkgconfig/libpq.pc.new" "$DEPS/lib/pkgconfig/libpq.pc"
  mark_done libpq "$POSTGRES_VERSION"
}

# Libraries for loadable extensions (scripts/build-ext.sh) are linked into
# a side module, so they are position-independent.
build_libsodium() {
  done_stamp libsodium "$SODIUM_VERSION" && return
  fetch "https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM_VERSION.tar.gz" "libsodium-$SODIUM_VERSION"
  # As libsodium's dist-build/wasm32-wasi.sh: no stack protector, no threads.
  # NDEBUG: php.wasm does not export __assert_fail.
  CFLAGS="$DEP_CFLAGS -fPIC -DNDEBUG" ./configure --host=wasm32-wasi --build="$BUILD_TRIPLE" --prefix="$DEPS" \
    --enable-static --disable-shared --disable-ssp --without-pthreads --disable-asm
  make -j"$NPROC"
  make install
  mark_done libsodium "$SODIUM_VERSION"
}

# zlib and OpenSSL come from php.wasm, which exports them: a side module
# resolves their symbols there. Only libzip's own code goes into zip.so.
build_libzip() {
  done_stamp libzip "$LIBZIP_VERSION" && return
  fetch "https://libzip.org/download/libzip-$LIBZIP_VERSION.tar.xz" "libzip-$LIBZIP_VERSION"
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$WASI_SDK/share/cmake/wasi-sdk-p1.cmake" -DWASI_SDK_PREFIX="$WASI_SDK" \
    -DCMAKE_C_FLAGS="$DEP_CFLAGS -fPIC -DNDEBUG" -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_OPENSSL=ON \
    -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_WINDOWS_CRYPTO=OFF >/dev/null
  cmake --build build -j"$NPROC"
  cmake --install build
  mark_done libzip "$LIBZIP_VERSION"
}

# compat_link prints linker arguments that link configure's test programs
# against compat/, as php.wasm will be: sockets, DNS and the rest are found.
compat_link() {
  local dir="$BUILD/compat-configure"
  mkdir -p "$dir"
  for src in "${COMPAT_SOURCES[@]}"; do
    $CC $DEP_CFLAGS -c "$COMPAT/$src.c" -o "$dir/$src.o"
  done
  # gophper_signal.c refers to Zend, which a test program does not have.
  echo 'void (*zend_interrupt_function)(void *);' > "$dir/zend_stub.c"
  $CC $CFLAGS -c "$dir/zend_stub.c" -o "$dir/zend_stub.o"
  local args=("$dir"/*.o)
  for fn in "${COMPAT_WRAPS[@]}"; do
    args+=("-Wl,--wrap=$fn")
  done
  echo "${args[*]}"
}

# libcurl, built into php.wasm with ext/curl. Sockets and DNS come from compat/.
build_curl() {
  done_stamp curl "$CURL_VERSION" && return
  local link
  link="$(compat_link)"
  fetch "https://curl.se/download/curl-$CURL_VERSION.tar.xz" "curl-$CURL_VERSION"
  # wasi-libc's headers hide these, and compat/ declares them only when
  # force-included, which configure cannot be.
  export curl_cv_func_socket=yes curl_cv_func_getaddrinfo=yes curl_cv_func_getaddrinfo_threadsafe=yes \
    curl_cv_func_getpeername=yes curl_cv_func_getsockname=yes curl_cv_func_freeaddrinfo=yes
  # Without the forced compat header: configure declares each function it
  # checks itself, and the header's declarations would conflict.
  CFLAGS="$CFLAGS -D_GNU_SOURCE -I$COMPAT/include -fPIC -DNDEBUG" LIBS="$link" CPPFLAGS="-I$DEPS/include" LDFLAGS="$LDFLAGS -L$DEPS/lib" \
    PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH="" \
    ./configure --host=wasm32-wasi --build="$BUILD_TRIPLE" --prefix="$DEPS" --enable-static --disable-shared \
    --with-openssl="$DEPS" --with-zlib="$DEPS" --with-ca-bundle=/etc/gophper/ssl/cert.pem \
    --disable-threaded-resolver --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
    --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-manual \
    --disable-docs --disable-ntlm --disable-kerberos-auth --without-libpsl --without-brotli --without-zstd \
    --without-nghttp2 --without-libidn2 --without-libssh2 --disable-ipv6
  # compat/ was in LIBS for configure only, and libtool refuses plain
  # objects in a library. A static library links nothing anyway.
  find . -name Makefile -exec sed -i.bak -e "s|$BUILD/compat-configure/[^ ]*||g" {} +
  make -C lib -j"$NPROC" CFLAGS="$DEP_CFLAGS -fPIC -DNDEBUG"
  make -C lib install
  make -C include install
  make install-pkgconfigDATA
  mark_done curl "$CURL_VERSION"
}

# For gd. zlib again comes from php.wasm.
build_libpng() {
  done_stamp libpng "$LIBPNG_VERSION" && return
  fetch "https://download.sourceforge.net/libpng/libpng-$LIBPNG_VERSION.tar.xz" "libpng-$LIBPNG_VERSION"
  CFLAGS="$DEP_CFLAGS -fPIC -DNDEBUG" CPPFLAGS="-I$DEPS/include" LDFLAGS="$LDFLAGS -L$DEPS/lib" \
    ./configure --host=wasm32-wasi --build="$BUILD_TRIPLE" --prefix="$DEPS" --enable-static --disable-shared \
    --disable-tools --disable-tests --disable-hardware-optimizations
  make -j"$NPROC" libpng16.la
  make install-libLTLIBRARIES install-pkgincludeHEADERS install-nodist_pkgincludeHEADERS install-header-links \
    install-pkgconfigDATA install-libpng-pc
  mark_done libpng "$LIBPNG_VERSION"
}

build_libjpeg() {
  done_stamp libjpeg "$LIBJPEG_TURBO_VERSION" && return
  fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEG_TURBO_VERSION/libjpeg-turbo-$LIBJPEG_TURBO_VERSION.tar.gz" "libjpeg-turbo-$LIBJPEG_TURBO_VERSION"
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$WASI_SDK/share/cmake/wasi-sdk-p1.cmake" -DWASI_SDK_PREFIX="$WASI_SDK" \
    -DCMAKE_C_FLAGS="$DEP_CFLAGS -fPIC -DNDEBUG" -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF >/dev/null
  cmake --build build -j"$NPROC"
  cmake --install build
  mark_done libjpeg "$LIBJPEG_TURBO_VERSION"
}

# ICU for intl. Cross-compiling ICU needs a native build of its tools, which
# also builds the data, from ICU's data sources, filtered by
# scripts/icu-filter.json: English and Japanese only. The data is an archive
# file, read at runtime from the extension directory: a static copy would
# fill every instance's memory with it.
build_icu() {
  local stamp
  stamp="$ICU_VERSION-$(shasum "$ROOT/scripts/icu-filter.json" | cut -c1-8)"
  done_stamp icu "$stamp" && return
  local src="$SRC/icu-$ICU_VERSION"
  local url="https://github.com/unicode-org/icu/releases/download/release-$ICU_VERSION"
  rm -rf "$src"
  mkdir -p "$src"
  echo "fetching ICU $ICU_VERSION"
  curl -fsSL "$url/icu4c-$ICU_VERSION-sources.tgz" | tar xz -C "$src"
  curl -fsSL -o "$src/data.zip" "$url/icu4c-$ICU_VERSION-data.zip"
  rm -rf "$src/icu/source/data"
  unzip -q "$src/data.zip" -d "$src/icu/source"
  local opts=(--disable-tests --disable-samples --disable-extras --disable-layoutex --disable-shared --enable-static
    --with-data-packaging=archive)

  mkdir -p "$src/host"
  (cd "$src/host" && env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS -u AR -u RANLIB -u NM \
    ICU_DATA_FILTER_FILE="$ROOT/scripts/icu-filter.json" "$src/icu/source/configure" "${opts[@]}" >/dev/null &&
    make -j"$NPROC" >/dev/null)

  mkdir -p "$src/wasm/lib" "$src/wasm/bin"
  cd "$src/wasm"
  # The data file is read from the extension directory, with stdio: WASI's
  # emulated mmap has no MAP_SHARED. WASI has no tzname or popen either, and
  # intl's own code reads date.timezone. A header carries the settings, as
  # quotes do not survive configure and make.
  cat > "$src/gophper-icu.h" <<'H'
#define U_ICU_DATA_DEFAULT_DIR "/usr/local/lib/php/extensions"
#define U_HAVE_MMAP 0
#define U_HAVE_POPEN 0
#define U_HAVE_TZNAME 0
#define U_HAVE_TIMEZONE 0
#define U_HAVE_TZSET 0
H
  local icu_flags="-fPIC -DNDEBUG -D_GNU_SOURCE -I$COMPAT/include -include $src/gophper-icu.h"
  CFLAGS="$CFLAGS $icu_flags" CXXFLAGS="$CXXFLAGS -fwasm-exceptions $icu_flags" \
  LDFLAGS="$LDFLAGS -fwasm-exceptions" icu_cv_host_frag=mh-linux \
    "$src/icu/source/configure" --host=wasm32-wasi --build="$BUILD_TRIPLE" --with-cross-build="$src/host" \
    "${opts[@]}" --disable-tools --disable-dyload >/dev/null
  for dir in common stubdata i18n; do
    make -C "$dir" -j"$NPROC"
  done
  mkdir -p "$DEPS/include/unicode" "$DEPS/share/icu"
  cp "$src"/icu/source/common/unicode/*.h "$src"/icu/source/i18n/unicode/*.h "$DEPS/include/unicode/"
  cp lib/libicuuc.a lib/libicui18n.a stubdata/libicudata.a "$DEPS/lib/"
  rm -f "$DEPS"/share/icu/*
  cp "$src/host/data/out/icudt${ICU_VERSION%%.*}l.dat" "$DEPS/share/icu/"
  mark_done icu "$stamp"
}

# libbz2: its Makefile builds tools too, so the library's sources are
# compiled here.
build_bzip2() {
  done_stamp bzip2 "$BZIP2_VERSION" && return
  fetch "https://sourceware.org/pub/bzip2/bzip2-$BZIP2_VERSION.tar.gz" "bzip2-$BZIP2_VERSION"
  local objs=()
  for src in blocksort huffman crctable randtable compress decompress bzlib; do
    $CC $DEP_CFLAGS -fPIC -DNDEBUG -c "$src.c" -o "$src.o"
    objs+=("$src.o")
  done
  rm -f libbz2.a
  "$AR" rcs libbz2.a "${objs[@]}"
  cp libbz2.a "$DEPS/lib/"
  cp bzlib.h "$DEPS/include/"
  mark_done bzip2 "$BZIP2_VERSION"
}

# GMP in plain C: no assembly for wasm.
build_gmp() {
  done_stamp gmp "$GMP_VERSION" && return
  fetch "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VERSION.tar.xz" "gmp-$GMP_VERSION"
  CFLAGS="$DEP_CFLAGS -fPIC -DNDEBUG" ./configure --host=none-unknown-none --build="$BUILD_TRIPLE" --prefix="$DEPS" \
    --enable-static --disable-shared --disable-assembly --disable-cxx
  make -j"$NPROC"
  make install
  mark_done gmp "$GMP_VERSION"
}

# phpredis, from PECL's GitHub. scripts/build-ext.sh builds it from here.
# scripts/ext/phpredis-session.patch follows PHP 8.6's session handler API.
build_phpredis() {
  local patch="$ROOT/scripts/ext/phpredis-session.patch" stamp
  stamp="$PHPREDIS_VERSION-$(shasum "$patch" | cut -c1-8)"
  done_stamp phpredis "$stamp" && return
  fetch "https://github.com/phpredis/phpredis/archive/refs/tags/$PHPREDIS_VERSION.tar.gz" "phpredis-$PHPREDIS_VERSION"
  patch -p1 < "$patch"
  mark_done phpredis "$stamp"
}

libs=("$@")
[[ ${#libs[@]} -gt 0 ]] || libs=(zlib libxml2 sqlite openssl libpq libsodium libzip curl libpng libjpeg icu bzip2 gmp phpredis)
for lib in "${libs[@]}"; do
  ( "build_$lib" )
done
ls -lh "$DEPS/lib"/*.a
