#!/usr/bin/env bash
# Download wasi-sdk, binaryen and php-src, then apply patches/.
# Usage: scripts/fetch.sh [--force]   (--force re-clones php-src)
set -euo pipefail

. "$(dirname "$0")/env.sh"

for cmd in git curl re2c bison autoconf; do
  command -v "$cmd" >/dev/null || { echo "missing: $cmd (macOS: brew install bison re2c autoconf pkgconf. Debian: apt install bison re2c autoconf pkgconf)" >&2; exit 1; }
done

if [[ ! -x "$WASI_SDK/bin/clang" ]]; then
  mkdir -p "$ROOT/.tools"
  major="${WASI_SDK_VERSION%%.*}"
  url="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$major/$(basename "$WASI_SDK").tar.gz"
  echo "downloading $url"
  curl -fsSL "$url" | tar xz -C "$ROOT/.tools"
  [[ "$(uname -s)" == Darwin ]] && xattr -dr com.apple.quarantine "$WASI_SDK" 2>/dev/null || true
fi

if [[ ! -x "$BINARYEN/bin/wasm-opt" ]]; then
  mkdir -p "$ROOT/.tools"
  case "$WASI_SDK_OS-$WASI_SDK_ARCH" in
    linux-arm64) platform=aarch64-linux ;;
    *) platform="$WASI_SDK_ARCH-$WASI_SDK_OS" ;;
  esac
  url="https://github.com/WebAssembly/binaryen/releases/download/version_$BINARYEN_VERSION/binaryen-version_$BINARYEN_VERSION-$platform.tar.gz"
  echo "downloading $url"
  curl -fsSL "$url" | tar xz -C "$ROOT/.tools"
  [[ "$(uname -s)" == Darwin ]] && xattr -dr com.apple.quarantine "$BINARYEN" 2>/dev/null || true
fi

if [[ "${1:-}" == "--force" ]]; then
  rm -rf "$PHP_SRC"
fi
if [[ -d "$PHP_SRC" ]]; then
  echo "php-src already exists; use --force to re-clone" >&2
  exit 0
fi

git clone -q --depth 1 --branch "$PHP_TAG" https://github.com/php/php-src.git "$PHP_SRC"
for p in "$ROOT"/patches/*.patch; do
  echo "applying $(basename "$p")"
  git -C "$PHP_SRC" apply --whitespace=nowarn "$p"
done
