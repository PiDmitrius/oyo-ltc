#!/usr/bin/env bash
#
# Build the PORTABLE oyo-web: linked inside the build image, stripped,
# static libcrypto — same baseline-shared-libs contract as the litecoind
# from scripts/02. Output: dist/out/oyo-web (consumed by scripts/07).
#
# Linking on the host would stamp the HOST's glibc/libstdc++ symbol versions
# into the binary (non-reproducible floor). The build image is Ubuntu 20.04 —
# the same g++ that compiled the cgo bundle (scripts/04) — so linking there
# pins the floor to glibc 2.31 / gcc-9 libstdc++ and matches the bundle ABI
# exactly. The Go toolchain is self-contained (no glibc needs of its own)
# and is mounted from the host read-only. The whole repo is mounted (not
# just oyo-web/) because the cgo package resolves liboyoltc/{include,lib}
# relative to the repo root.
set -euo pipefail

cd "$(dirname "$0")/.."

test -f liboyoltc/lib/liboyoltc_bundle.a || {
  echo "no liboyoltc bundle — run scripts/04-build-liboyoltc.sh first" >&2
  exit 1
}

GOROOT_HOST="$(go env GOROOT)"
GOMODCACHE_HOST="$(go env GOMODCACHE)"
mkdir -p "$GOMODCACHE_HOST"
# Mount GOROOT at its native path so internal RELATIVE symlinks still resolve
# (Debian's /usr/lib/go-X is half symlinks into /usr/share/go-X — mount that
# target too when it lives outside GOROOT).
GOROOT_MOUNTS=(-v "$GOROOT_HOST:$GOROOT_HOST:ro")
SRC_REAL="$(readlink -f "$GOROOT_HOST/src")"
SRC_PARENT="${SRC_REAL%/src}"
[ "$SRC_PARENT" != "$GOROOT_HOST" ] && GOROOT_MOUNTS+=(-v "$SRC_PARENT:$SRC_PARENT:ro")

mkdir -p dist/out
docker run --rm -u "$(id -u):$(id -g)" \
  "${GOROOT_MOUNTS[@]}" \
  -v "$GOMODCACHE_HOST:/opt/gomod:ro" \
  -v "$(pwd):/src" \
  -e PATH="$GOROOT_HOST/bin:/usr/bin:/bin" -e GOROOT="$GOROOT_HOST" \
  -e GOMODCACHE=/opt/gomod -e GOFLAGS=-mod=readonly \
  -e GOCACHE=/tmp/gocache -e HOME=/tmp -e CGO_ENABLED=1 \
  --entrypoint /bin/bash litecoin-oyo-build \
  -c 'cd /src/oyo-web && go build -a -trimpath -ldflags="-s -w" -o /src/dist/out/oyo-web .'

ls -lh dist/out/oyo-web
