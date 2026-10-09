---
name: php-upgrade
description: Move gophper-wasm to a new php-src release, such as 8.6.0RC3 to 8.6.0 or 8.6.x. Use when PHP_TAG in scripts/env.sh changes, or when asked to update PHP itself.
---

# Upgrade PHP

## Steps

1. Set `PHP_TAG` in `scripts/env.sh`, such as `php-8.6.1`. Tags are listed at https://github.com/php/php-src/tags.
2. Clone it again and apply `patches/`:

   ```sh
   scripts/fetch.sh --force
   ```

   A patch that no longer applies stops the script. Apply the rest by hand, in order, with `git -C php-src apply --reject`. Fix the rejected parts in `php-src/`, keep each `GOPHPER:` marker, then run `scripts/export-patches.sh`. Read the php-src-patch skill first.
3. Build from scratch. A new tag changes configure, so reconfigure:

   ```sh
   scripts/build.sh --reconfigure && scripts/build-ext.sh
   ```

4. Check what PHP's own changes may break:

   | Where | What to check |
   | --- | --- |
   | `php-src/config.log` | `error parsing wasm`. A configure test that wasm-opt cannot parse looks like a missing function. |
   | `scripts/ext/phpredis.h`, `scripts/ext/phpredis-session.patch` | APIs PHP removed or changed. phpredis lags behind new PHP releases. |
   | `patches/0009` | opcache's file cache, which changes between releases |
   | `ext/opcache/zend_shared_alloc.c` (`patches/0011`) | Whether opcache still creates its lock file there |
   | `ext/openssl/xp_ssl.c` (`patches/0012`) | Whether php-src fixed the `STREAM_XPORT_OP_LISTEN` return code itself. Then drop the patch. |
   | `UPGRADING` and `UPGRADING.INTERNALS` in php-src | New functions that need compat, or changed SAPI behavior |

5. Test: `go vet ./... && go test ./...` here, then gophper's whole suite against this checkout (gophper's `gophper-wasm-upgrade` skill, "Before a release").
6. Update PHP's row in README's "Versions inside" table. No other place in either README names PHP's version.
7. Release as a patch version (release skill).
