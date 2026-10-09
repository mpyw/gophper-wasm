#!/usr/bin/env bash
# Build a php-src extension as a wasm side module that php.wasm loads at
# runtime, like a .so. Run scripts/build.sh first: this compiles against its
# configured headers.
# Usage: scripts/build-ext.sh [extension...]   (default: SHARED_EXTENSIONS in env.sh)
# Writes .build/extensions/<extension>.so, and the committed ext/<extension>.so.gz.
set -euo pipefail

. "$(dirname "$0")/env.sh"
if [[ $# -eq 0 ]]; then
  set -- "${SHARED_EXTENSIONS[@]}"
fi
if [[ $# -gt 1 ]]; then
  for ext in "$@"; do
    "$0" "$ext"
  done
  exit 0
fi
COMPAT="$ROOT/compat"
EXT="$1"
SRC="$PHP_SRC/ext/$EXT"
# PECL extensions, which scripts/deps.sh fetches.
case "$EXT" in
  redis) SRC="$BUILD/deps-src/phpredis-$PHPREDIS_VERSION" ;;
esac
OUT="$BUILD/extensions"
OBJ="$BUILD/ext-obj/$EXT"
[[ -f "$SRC/config.m4" ]] || { echo "no such extension: $EXT" >&2; exit 1; }

# The sources are the second argument of PHP_NEW_EXTENSION in config.m4.
sources="$(tr '\n' ' ' < "$SRC/config.m4" | sed -nE 's/.*PHP_NEW_EXTENSION\(\[?'"$EXT"'\]?, *\[?([^],]+)\]?,.*/\1/p')"

upper="$(echo "$EXT" | tr '[:lower:]' '[:upper:]')"

# What an extension needs from scripts/deps.sh, linked into its .so.
EXT_CFLAGS=()
EXT_LIBS=()
# Sources other than config.m4's literal list, for an extension that builds them up.
EXT_SOURCES=""
# Files the extension needs at runtime, shipped as ext/<extension>-<file>.gz.
EXT_FILES=()
case "$EXT" in
  sodium)
    EXT_CFLAGS=(-I"$DEPS/include")
    EXT_LIBS=("$DEPS/lib/libsodium.a")
    ;;
  gd)
    # The libgd bundled in php-src, with PNG and JPEG. zlib comes from php.wasm.
    # config.m4 lists them in extra_sources. The directory holds test programs too.
    EXT_SOURCES="gd.c $(sed -n '/extra_sources=m4_normalize/,/"\])/p' "$SRC/config.m4" | grep -o 'libgd/[^ ]*\.c')"
    EXT_CFLAGS=(-I"$DEPS/include" -I"$SRC/libgd" -Wno-strict-prototypes
      -DHAVE_GD_BUNDLED=1 -DHAVE_GD_GET_INTERPOLATION=1 -DHAVE_GD_PNG_GET_VERSION_STRING=1
      -DHAVE_LIBZ=1 -DHAVE_LIBPNG=1 -DHAVE_GD_PNG=1 -DHAVE_LIBJPEG=1 -DHAVE_GD_JPG=1
      -DHAVE_ICONV_H=1 -DHAVE_ICONV_T_DEF=1)
    EXT_LIBS=("$DEPS/lib/libpng16.a" "$DEPS/lib/libjpeg.a")
    ;;
  intl)
    # ICU, and C++ with exceptions: libc++ goes into intl.so. ICU reads its
    # data from the extension directory, so the data file ships beside it.
    EXT_SOURCES="$(sed -n '/PHP_NEW_EXTENSION(\[intl\]/,/\]),/p' "$SRC/config.m4" | grep -oE '[a-z_/]+\.c\b')
      $(sed -n '/PHP_INTL_CXX_SOURCES="/,/"$/p' "$SRC/config.m4" | grep -oE '[a-z_/]+\.cpp')"
    EXT_CFLAGS=(-I"$DEPS/include" -Wno-write-strings -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS
      -D__STDC_FORMAT_MACROS -DU_NO_DEFAULT_INCLUDE_UTF_HEADERS=1 -DU_HIDE_OBSOLETE_UTF_OLD_H=1)
    # Whole: inline functions such as UnicodeString::operator== are weak
    # definitions in ICU's archives, which the linker does not pull in, and
    # they would be left as imports that nothing provides.
    EXT_LIBS=(-Wl,--whole-archive,"$DEPS/lib/libicui18n.a","$DEPS/lib/libicuuc.a",--no-whole-archive "$DEPS/lib/libicudata.a")
    EXT_FILES=("$DEPS/share/icu/icudt${ICU_VERSION%%.*}l.dat")
    ;;
  bz2)
    EXT_CFLAGS=(-I"$DEPS/include" -DHAVE_BZ2=1)
    EXT_LIBS=("$DEPS/lib/libbz2.a")
    ;;
  gmp)
    EXT_CFLAGS=(-I"$DEPS/include" -DHAVE_GMP=1)
    EXT_LIBS=("$DEPS/lib/libgmp.a")
    ;;
  redis)
    # Sessions (session.save_handler=redis) and the JSON serializer. No
    # igbinary, msgpack or compression.
    EXT_SOURCES="redis.c redis_commands.c library.c redis_session.c redis_array.c redis_array_impl.c
      redis_cluster.c cluster_library.c redis_sentinel.c sentinel_library.c backoff.c"
    # scripts/ext/phpredis.h maps what PHP 8.6 removed.
    EXT_CFLAGS=(-DPHP_SESSION=1 -DHAVE_REDIS_JSON=1 -include "$ROOT/scripts/ext/phpredis.h")
    ;;
  zip)
    # What ext/zip/config.m4 finds in libzip 1.11.
    EXT_CFLAGS=(-I"$DEPS/include" -DHAVE_ZIP=1 -DHAVE_ENCRYPTION=1 -DHAVE_LIBZIP_VERSION=1
      -DHAVE_PROGRESS_CALLBACK=1 -DHAVE_CANCEL_CALLBACK=1 -DHAVE_METHOD_SUPPORTED=1)
    EXT_LIBS=("$DEPS/lib/libzip.a")
    ;;
esac
mkdir -p "$OUT" "$OBJ"
objs=()
if [[ -n "$EXT_SOURCES" ]]; then
  sources="$EXT_SOURCES"
fi
[[ -n "$sources" ]] || { echo "$EXT: cannot read the sources from config.m4" >&2; exit 1; }
cxx=false
for src in $sources; do
  obj="$OBJ/${src//\//_}.o"
  compile=($CC $CFLAGS)
  if [[ "$src" == *.cpp ]]; then
    # C++ exceptions in the standard (exnref) encoding, as setjmp uses.
    compile=($CXX $CXXFLAGS -std=gnu++17 -fwasm-exceptions)
    cxx=true
  fi
  "${compile[@]}" -fPIC -fvisibility=default \
    -D_GNU_SOURCE -I"$COMPAT/include" -include "$COMPAT/gophper_compat.h" \
    -DCOMPILE_DL_"$upper" -DZEND_COMPILE_DL_EXT=1 \
    -I"$SRC" -I"$PHP_SRC" -I"$PHP_SRC/main" -I"$PHP_SRC/Zend" -I"$PHP_SRC/TSRM" -I"$PHP_SRC/ext" -I"$PHP_SRC/ext/date/lib" \
    "${EXT_CFLAGS[@]}" -c "$SRC/$src" -o "$obj"
  objs+=("$obj")
done

# Undefined symbols become imports, which the host resolves against php.wasm.
# php.wasm carries the C++ runtime (scripts/build.sh): a C++ extension
# imports it, as it does libc.
link=($CC)
if $cxx; then
  link=($CXX -fwasm-exceptions -nostdlib++)
fi
"${link[@]}" -shared -fPIC -Wl,--experimental-pic -Wl,--unresolved-symbols=import-dynamic \
  -o "$OUT/$EXT.so" "${objs[@]}" "${EXT_LIBS[@]}"
# --strip-debug keeps the dylink.0 section the host links with.
"$WASI_SDK/bin/llvm-strip" --strip-debug "$OUT/$EXT.so"
gzip -9 -n -c "$OUT/$EXT.so" > "$ROOT/ext/$EXT.so.gz"
for f in "${EXT_FILES[@]}"; do
  gzip -9 -n -c "$f" > "$ROOT/ext/$EXT-$(basename "$f").gz"
done
ls -lh "$OUT/$EXT.so" "$ROOT/ext/$EXT.so.gz"
