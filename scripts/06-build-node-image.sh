#!/usr/bin/env bash
#
# Build the litecoin-oyo node runtime image from the portable static
# litecoind produced by scripts/02 (oyo-ltc-node-static volume). The binary
# is static — the image needs no shared-library extraction, just the one
# file. Stripped on the way in: the dev stand never needs node debug
# symbols, and the release archive strips identically (scripts/07).
set -euo pipefail

cd "$(dirname "$0")/.."

STAGING=$(mktemp -d /tmp/oyo-ltc-node-stage-XXXXXX)
cleanup() { rm -rf "$STAGING"; }
trap cleanup EXIT

echo "=== Extracting litecoind (static volume) ==="
docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v oyo-ltc-node-static:/work:ro \
  -v "$STAGING:/out" \
  --entrypoint bash \
  litecoin-oyo-build -c '
set -e
test -x /work/litecoin-oyo-fork/src/litecoind || {
  echo "no litecoind in oyo-ltc-node-static — run scripts/02-build-litecoind.sh first" >&2
  exit 1
}
cp /work/litecoin-oyo-fork/src/litecoind /out/litecoind
strip /out/litecoind
ls -lh /out/litecoind
'

cp docker/Dockerfile.node "$STAGING/Dockerfile"

echo "=== Building litecoin-oyo image ==="
docker build -t litecoin-oyo "$STAGING"

echo "=== Done ==="
docker images litecoin-oyo
