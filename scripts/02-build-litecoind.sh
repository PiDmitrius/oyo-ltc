#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

docker run --rm \
  -e JOBS="${JOBS:-}" \
  -v "$(pwd):/src:ro" \
  -v oyo-ltc-node-work:/work \
  litecoin-oyo-build \
  bash -lc '
set -euo pipefail

src=/src/litecoin-oyo-fork
work=/work/litecoin-oyo-fork
jobs="${JOBS:-$(nproc)}"

if [ ! -d "$src" ]; then
  echo "missing submodule at $src" >&2
  exit 1
fi

echo "=== Syncing source ==="
mkdir -p "$work"
rsync -a --checksum --exclude=".git" "$src/" "$work/"

cd "$work"

echo "=== Environment ==="
gcc --version | head -1
python3 --version
echo "jobs: $jobs"
echo

if [ ! -f configure ]; then
  echo "=== autogen ==="
  ./autogen.sh
fi

if [ ! -f Makefile ]; then
  echo "=== configure ==="
  export BDB_PREFIX=/opt/db4
  ./configure --without-gui --disable-bench \
    BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" \
    BDB_CFLAGS="-I${BDB_PREFIX}/include"
fi

echo "=== build litecoind ==="
make -j"$jobs" src/litecoind
ls -lh src/litecoind
'
