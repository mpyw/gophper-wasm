#!/usr/bin/env bash
# Build php-src (already fetched and patched) into php.wasm.gz and php-cgi.wasm.gz.
# Usage: scripts/build.sh [--reconfigure]
set -euo pipefail

. "$(dirname "$0")/env.sh"
COMPAT="$ROOT/compat"

# 1. POSIX stubs that wasi-libc lacks.
mkdir -p "$BUILD/compat"
for src in "${COMPAT_SOURCES[@]}"; do
  $CC $CFLAGS -D_GNU_SOURCE -I"$COMPAT/include" -include "$COMPAT/gophper_compat.h" \
    -c "$COMPAT/$src.c" -o "$BUILD/compat/$src.o"
done

# 2. configure (only once unless --reconfigure).
cd "$PHP_SRC"
if [[ ! -f Makefile || "${1:-}" == "--reconfigure" ]]; then
  rm -f config.cache
  ./buildconf --force
  # These link only against compat/, which configure does not see. Telling
  # configure they exist enables PHP's getaddrinfo and service lookups.
  export php_cv_func_getaddrinfo=yes ac_cv_func_gai_strerror=yes \
    ac_cv_func_getprotobyname=yes ac_cv_func_getprotobynumber=yes \
    ac_cv_func_getservbyname=yes ac_cv_func_getservbyport=yes
  # libpq links against compat/ (signal masks) too. It is PostgreSQL 18.
  # libcurl, too, needs compat/ for its sockets.
  export ac_cv_lib_curl_curl_easy_perform=yes
  export ac_cv_lib_pq_PQencryptPasswordConn=yes ac_cv_lib_pq_PQresultMemorySize=yes \
    ac_cv_lib_pq_PQclosePrepared=yes ac_cv_lib_pq_PQclosePortal=yes ac_cv_lib_pq_PQsetChunkedRowsMode=yes \
    ac_cv_lib_pq_PQchangePassword=yes ac_cv_lib_pq_PQsocketPoll=yes
  # compat/gophper_proc.c starts processes on the host. fork itself still
  # fails, but configure enables proc_open only when it finds fork. PHP then
  # uses posix_spawn, since posix_spawn_file_actions_addchdir is there.
  export ac_cv_func_fork=yes ac_cv_func_posix_spawn_file_actions_addchdir=yes
  # compat/gophper_sys.c locks files on the host.
  export ac_cv_func_flock=yes
  # compat/gophper_signal.c: sigaction and the rest, for pcntl.
  export ac_cv_func_sigaction=yes ac_cv_func_waitpid=yes ac_cv_func_sigprocmask=yes \
    ac_cv_func_sigwaitinfo=yes ac_cv_func_sigtimedwait=yes ac_cv_func_alarm=yes
  # More of compat/: users, groups, sessions, limits and the rest.
  export ac_cv_func_getgroups=yes ac_cv_func_getlogin=yes ac_cv_func_initgroups=yes ac_cv_func_lchown=yes \
    ac_cv_func_socketpair=yes ac_cv_func_getpgid=yes ac_cv_func_getsid=yes ac_cv_func_setsid=yes \
    ac_cv_func_ctermid=yes ac_cv_func_nice=yes ac_cv_func_getloadavg=yes ac_cv_func_seteuid=yes \
    ac_cv_func_setegid=yes ac_cv_func_getrlimit=yes ac_cv_func_setrlimit=yes ac_cv_func_mkfifo=yes \
    ac_cv_func_mknod=yes
  # wasi-libc has getrusage, but configure's test fails: wasm-opt cannot
  # parse a test program that calls it with the wrong prototype.
  export ac_cv_func_getrusage=yes
  # opcache's shared memory: an anonymous mmap, which compat/ makes private.
  export php_cv_shm_mmap_anon=yes
  # compat/gophper_dns.c: res_search through the host, for dns_get_record().
  # patches/0007 defines the functions, which PHP_CHECK_FUNC checks anew.
  export ac_cv_header_resolv_h=yes
  # Headers in compat/include, which configure does not see either.
  export ac_cv_header_sys_wait_h=yes ac_cv_header_pwd_h=yes ac_cv_header_grp_h=yes ac_cv_header_syslog_h=yes \
    ac_cv_header_sys_resource_h=yes
  # Extensions built in. --disable-all turns off everything else.
  EXTENSIONS=(
    # Bundled, no external library.
    --enable-bcmath --enable-calendar --enable-ctype --enable-exif
    --enable-fileinfo --enable-filter --enable-pdo --enable-phar
    --enable-pcntl --enable-posix --enable-session --enable-tokenizer
    # mb_ereg* needs oniguruma, which is not built.
    --enable-mbstring --disable-mbregex
    # wasi-libc's iconv (from musl).
    --with-iconv
    # MySQL.
    --enable-mysqlnd --with-mysqli --with-pdo-mysql
    # Built by scripts/deps.sh into .build/deps.
    --with-zlib --with-openssl --with-sqlite3 --with-pdo-sqlite --with-pdo-pgsql --with-pgsql --with-curl
    --with-libxml --enable-dom --enable-xml --enable-simplexml --enable-xmlreader --enable-xmlwriter
  )
  # configure finds the libraries through pkg-config, and only ours.
  export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH=""
  # Shared extensions are wasm side modules, loaded by the host (compat/gophper_dl.c).
  export EXTENSION_DIR=/usr/local/lib/php/extensions
  PHP_UNAME="WASI wasm32" ./configure -C --host=wasm32-wasi --build="$BUILD_TRIPLE" \
    --disable-all "${EXTENSIONS[@]}" --enable-cli --enable-cgi --disable-phpdbg --disable-fpm \
    --disable-fiber-asm --disable-zend-signals --without-pcre-jit \
    --disable-shared --enable-static --without-valgrind --disable-gcc-global-regs \
    --with-config-file-path=/etc/gophper
fi

# 3. make. The compat header is force-included only here, not during configure.
# Force-included headers are missing from the .dep files, so rebuild everything
# whenever compat/ changes.
STAMP="$BUILD/compat.stamp"
HASH="$(find "$COMPAT" -type f -print0 | sort -z | xargs -0 shasum | shasum | cut -d' ' -f1)"
if [[ "$(cat "$STAMP" 2>/dev/null)" != "$HASH" ]]; then
  make clean >/dev/null
fi
# gophper_zend.c and gophper_fn.c need the configured php-src headers, so
# they are built here.
COMPAT_CFLAGS="-D_GNU_SOURCE -I$COMPAT/include -include $COMPAT/gophper_compat.h"
for src in gophper_zend gophper_fn; do
  $CC $CFLAGS $COMPAT_CFLAGS -I"$PHP_SRC" -I"$PHP_SRC/main" -I"$PHP_SRC/Zend" -I"$PHP_SRC/TSRM" \
    -c "$COMPAT/$src.c" -o "$BUILD/compat/$src.o"
done
# Dynamic linking: export every symbol, the function table and the stack
# pointer, so that side modules can link against them. All of libc goes in,
# since an extension may use a function PHP itself does not.
# wasm-ld reads an archive only once, and clang already passes -lc, so
# --whole-archive -lc would be ignored. A copy under another name is not.
# All three go in one -Wl: libtool moves archives away from other flags.
# The copy leaves out the members that define what compat/ defines itself,
# which would be duplicates now.
cp "$SYSROOT/lib/wasm32-wasip1/libc.a" "$BUILD/libc-whole.a"
"$NM" -g --defined-only "$BUILD"/compat/*.o | awk 'NF == 3 { print $3 }' | sort -u > "$BUILD/compat-symbols.txt"
mapfile -t clash < <("$NM" -A -g --defined-only "$BUILD/libc-whole.a" 2>/dev/null |
  awk 'FNR == NR { c[$1] = 1; next } NF == 4 && ($4 in c) { split($1, p, ":"); print p[2] }' "$BUILD/compat-symbols.txt" - | sort -u)
if [[ ${#clash[@]} -gt 0 ]]; then
  "$AR" d "$BUILD/libc-whole.a" "${clash[@]}"
fi
# zlib and OpenSSL go in whole too, so that extensions can use them: zip.so
# and curl.so link against these copies in php.wasm.
WHOLE="$BUILD/libc-whole.a"
for lib in z ssl crypto; do
  cp "$DEPS/lib/lib$lib.a" "$BUILD/lib$lib-whole.a"
  WHOLE="$WHOLE,$BUILD/lib$lib-whole.a"
done
# The C++ runtime, with exceptions, for C++ extensions such as intl.so:
# wasi-sdk's libc++ is not position-independent, so it cannot go in a side
# module. php.wasm itself has no C++.
for lib in c++ c++abi unwind; do
  cp "$SYSROOT/lib/wasm32-wasip1/eh/lib$lib.a" "$BUILD/lib$lib-whole.a"
  WHOLE="$WHOLE,$BUILD/lib$lib-whole.a"
done
DYLINK="-Wl,--export-all -Wl,--export-table -Wl,--growable-table -Wl,--export=__stack_pointer -Wl,--whole-archive,$WHOLE,--no-whole-archive"
EXPORTS="-Wl,--export=gophper_vm_interrupt -Wl,--export=gophper_timed_out -Wl,--export=gophper_hard_timeout"
# compat/gophper_net.c takes these over for socket fds, compat/gophper_fd.c
# for dup aliases and file paths, and compat/gophper_proc.c for pipes.
COMPAT_OBJS=""
for src in "${COMPAT_SOURCES[@]}"; do
  COMPAT_OBJS="$COMPAT_OBJS $BUILD/compat/$src.o"
done
WRAPS=""
for fn in "${COMPAT_WRAPS[@]}"; do
  WRAPS="$WRAPS -Wl,--wrap=$fn"
done

# Appended to configure's EXTRA_LIBS, which holds the libraries it found.
CONFIGURED_LIBS="$(sed -n 's/^EXTRA_LIBS = //p' Makefile)"
# Link flags are not in make's dependencies, so always link again. It is quick.
rm -f sapi/cli/php sapi/cgi/php-cgi
make -j"$NPROC" cli cgi \
  EXTRA_CFLAGS="$COMPAT_CFLAGS" \
  EXTRA_LIBS="$CONFIGURED_LIBS $COMPAT_OBJS $BUILD/compat/gophper_zend.o $BUILD/compat/gophper_fn.o $EXPORTS $WRAPS $DYLINK"

echo "$HASH" > "$STAMP"

cp sapi/cli/php "$BUILD/php.wasm"
cp sapi/cgi/php-cgi "$BUILD/php-cgi.wasm"

# 4. The committed copies, embedded by phpwasm.go. Users of the Go package
# then need no C toolchain.
# Debug info is stripped. gzip -n leaves out the timestamp, so the .gz
# changes only when the wasm does.
for name in php php-cgi; do
  "$WASI_SDK/bin/llvm-strip" -o "$BUILD/$name.stripped.wasm" "$BUILD/$name.wasm"
  gzip -9 -n -c "$BUILD/$name.stripped.wasm" > "$ROOT/$name.wasm.gz"
done
sum() { shasum -a 256 "$1" | cut -c1-16; }
cat > "$ROOT/version.go" <<GO
// Code generated by scripts/build.sh. DO NOT EDIT.

package phpwasm

// PHPVersion is the php-src release the binaries are built from.
const PHPVersion = "${PHP_TAG#php-}"

// CLIDigest and CGIDigest identify the binaries, without decompressing
// them. A host may keep CLI() or CGI() on disk under its digest, since
// decompressing takes longer than reading.
const (
	CLIDigest = "$(sum "$BUILD/php.stripped.wasm")"
	CGIDigest = "$(sum "$BUILD/php-cgi.stripped.wasm")"
)
GO
ls -lh "$BUILD"/*.wasm "$ROOT"/*.wasm.gz
