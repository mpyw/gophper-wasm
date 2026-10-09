# Source this to get the wasi-sdk cross toolchain environment.
# Versions are pinned here.
PHP_TAG="php-8.6.0RC3"
WASI_SDK_VERSION="34.0"
# clang runs wasm-opt from PATH after linking, so its version shapes the output.
BINARYEN_VERSION="133"
# Libraries built by scripts/deps.sh.
ZLIB_VERSION="1.3.2"
LIBXML2_VERSION="2.15.4"
OPENSSL_VERSION="4.0.3"
SQLITE_VERSION="3530400" # 3.53.4, as sqlite.org numbers its downloads
SQLITE_YEAR="2026"
POSTGRES_VERSION="18.6" # only libpq is built
# Libraries for loadable extensions, built as PIC.
SODIUM_VERSION="1.0.22"
LIBZIP_VERSION="1.11.4"
# The loadable extensions in ext/, built by scripts/build-ext.sh.
SHARED_EXTENSIONS=(dl_test sodium zip gd intl bz2 gmp redis)
CURL_VERSION="8.22.0"
LIBPNG_VERSION="1.6.58"
LIBJPEG_TURBO_VERSION="3.2.0"
ICU_VERSION="78.3"
BZIP2_VERSION="1.0.8"
GMP_VERSION="6.3.0"
PHPREDIS_VERSION="6.3.0"

# The compat/ sources, and the calls they take over with --wrap.
COMPAT_SOURCES=(gophper_compat gophper_net gophper_dl gophper_fd gophper_proc gophper_sys gophper_dns gophper_signal)
COMPAT_WRAPS=(read write readv writev recv send close poll select fcntl ioctl accept accept4 shutdown
  fstat lseek ftruncate fsync fdatasync futimens isatty fstatvfs fdopendir dup dup2 dup3
  open pipe pipe2 uname gethostname stat lstat access chmod fchmod raise mmap)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Work directories, all ignored by Git.
PHP_SRC="$ROOT/php-src"
BUILD="$ROOT/.build"
# Where scripts/deps.sh installs the libraries.
DEPS="$BUILD/deps"
case "$(uname -m)" in
  arm64 | aarch64) WASI_SDK_ARCH=arm64 ;;
  *) WASI_SDK_ARCH=x86_64 ;;
esac
case "$(uname -s)" in
  Darwin) WASI_SDK_OS=macos ;;
  *) WASI_SDK_OS=linux ;;
esac
export WASI_SDK="$ROOT/.tools/wasi-sdk-$WASI_SDK_VERSION-$WASI_SDK_ARCH-$WASI_SDK_OS"
BINARYEN="$ROOT/.tools/binaryen-version_$BINARYEN_VERSION"
export PATH="$BINARYEN/bin:$PATH"
NPROC="$(getconf _NPROCESSORS_ONLN)"
# configure's --build. The binaries do not depend on it.
case "$(uname -s)" in
  Darwin) BUILD_TRIPLE="$(uname -m)-apple-darwin" ;;
  *) BUILD_TRIPLE="$(uname -m)-pc-linux-gnu" ;;
esac

# php-src needs bison >= 3.0; macOS ships 2.3.
if command -v brew >/dev/null && [[ -d "$(brew --prefix bison 2>/dev/null)/bin" ]]; then
  export PATH="$(brew --prefix bison)/bin:$PATH"
fi

SYSROOT="$WASI_SDK/share/wasi-sysroot"
export CC="$WASI_SDK/bin/clang --target=wasm32-wasip1 --sysroot=$SYSROOT"
export CXX="$WASI_SDK/bin/clang++ --target=wasm32-wasip1 --sysroot=$SYSROOT"
export AR="$WASI_SDK/bin/llvm-ar" RANLIB="$WASI_SDK/bin/llvm-ranlib" NM="$WASI_SDK/bin/llvm-nm"

WASI_EMU="-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID"
# setjmp/longjmp through the standard (exnref) exception handling proposal.
# wazero accepts only this encoding, not the legacy one.
SJLJ="-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
# Source paths in the binaries (__FILE__, debug info) start at the repository,
# so they do not depend on where it is checked out.
PREFIX_MAP="-ffile-prefix-map=$ROOT=."
export CFLAGS="-O2 $WASI_EMU $SJLJ $PREFIX_MAP"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-mman -lwasi-emulated-getpid -lsetjmp -Wl,--max-memory=4294967296 -Wl,-z,stack-size=8388608"
