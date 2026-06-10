#!/usr/bin/env bash
#
# Build the PORTABLE litecoind: boost/openssl/libevent/zmq/sqlite/bdb compiled
# from the in-tree depends/ system and linked statically, so the binary only
# needs baseline shared libs (glibc + libstdc++/libgcc) present on every
# Linux. This one binary serves both the release archive (scripts/07) and the
# node runtime image (scripts/06) — what e2e tests is what ships.
#
# A dynamically-linked litecoind is never a deliverable: its .so deps are
# version-pinned to the build image. The dynamic tree still exists, but only
# as an internal detail of scripts/03 (node tests) and scripts/04 (cgo
# bundle), which own the oyo-ltc-node-work volume and build it themselves.
#
# This script uses its own volume (oyo-ltc-node-static) so the static
# configure/objects never collide with that dynamic tree. First run builds
# depends/ (~30-60 min); the result is cached on the volume, subsequent runs
# only recompile what changed.
set -euo pipefail

cd "$(dirname "$0")/.."

docker run --rm \
  -e JOBS="${JOBS:-}" \
  -v "$(pwd):/src:ro" \
  -v oyo-ltc-node-static:/work \
  litecoin-oyo-build \
  bash -lc '
set -euo pipefail

src=/src/litecoin-oyo-fork
work=/work/litecoin-oyo-fork
jobs="${JOBS:-$(nproc)}"
triplet=x86_64-pc-linux-gnu

if [ ! -d "$src" ]; then
  echo "missing submodule at $src" >&2
  exit 1
fi

echo "=== Syncing source ==="
mkdir -p "$work"
rsync -a --checksum --exclude=".git" "$src/" "$work/"

cd "$work"
depends_prefix="$PWD/depends/$triplet"

echo "=== Environment ==="
gcc --version | head -1
echo "jobs: $jobs"
echo

if [ ! -f "$depends_prefix/share/config.site" ]; then
  echo "=== Building depends/ for $triplet (one-time, ~30-60 min) ==="
  make -C depends HOST="$triplet" -j"$jobs" NO_QT=1
else
  echo "=== Reusing cached depends/$triplet ==="
fi

if [ ! -f configure ]; then
  echo "=== autogen ==="
  ./autogen.sh
fi

if [ ! -f Makefile ]; then
  echo "=== configure (static, depends/) ==="
  CONFIG_SITE="$depends_prefix/share/config.site" \
  ./configure --without-gui --disable-bench --prefix=/
fi

echo "=== build litecoind ==="
make -j"$jobs" src/litecoind
ls -lh src/litecoind
'
