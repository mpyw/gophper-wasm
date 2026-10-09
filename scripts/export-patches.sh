#!/usr/bin/env bash
# Write the php-src working tree changes back to patches/, one file per patched source.
# Every change must carry a "GOPHPER:" marker comment.
set -euo pipefail

. "$(dirname "$0")/env.sh"
cd "$PHP_SRC"

patch_name() {
  case "$1" in
    configure.ac) echo 0001-configure-wasi ;;
    Zend/zend_fibers.c) echo 0002-zend-fibers-unsupported-on-wasi ;;
    Zend/zend_alloc.c) echo 0003-zend-alloc-aligned-alloc-on-wasi ;;
    Zend/zend_execute_API.c) echo 0004-zend-timeout-through-go-host ;;
    ext/random/csprng.c) echo 0005-random-from-wasi-random-get ;;
    ext/standard/microtime.c) echo 0006-getrusage-fields-on-wasi ;;
    ext/standard/config.m4) echo 0007-standard-functions-on-wasi ;;
    Zend/zend.c) echo 0008-zend-signals-through-go-host ;;
    ext/opcache/zend_file_cache.c) echo 0009-opcache-file-cache-attributed-const-on-32-bit ;;
    main/main.c) echo 0010-functions-from-go-host ;;
    ext/opcache/zend_shared_alloc.c) echo 0011-opcache-lock-file-on-wasi ;;
    ext/openssl/xp_ssl.c) echo 0012-openssl-listen-failure ;;
    main/streams/xp_socket.c) echo 0013-listen-error-text ;;
    Zend/zend_execute.c) echo 0014-yield-to-the-go-scheduler ;;
    main/fastcgi.c) echo 0015-fastcgi-shutdown-writes ;;
  esac
}

for file in $(git diff --name-only); do
  name="$(patch_name "$file")"
  if [[ -z "$name" ]]; then
    echo "no patch name for $file; add it to scripts/export-patches.sh" >&2
    exit 1
  fi
  if ! git diff -- "$file" | grep -q 'GOPHPER:'; then
    echo "$file: change has no GOPHPER: marker" >&2
    exit 1
  fi
  git diff -- "$file" > "$ROOT/patches/$name.patch"
  echo "wrote patches/$name.patch"
done
