#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

docker run --rm -i \
  -e JOBS="${JOBS:-}" \
  -v "$(pwd):/src:ro" \
  -v oyo-ltc-node-work:/work \
  litecoin-oyo-build \
  bash -s -- "$@" <<'SCRIPT'
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

echo "=== make check ==="
make -j"$jobs" check

if [ "$#" -eq 0 ]; then
  echo "=== quick functional tests ==="
  set -- \
    rpc_oyo.py \
    mweb_basic.py \
    mweb_mining.py \
    mweb_wallet_basic.py \
    wallet_basic.py \
    wallet_send.py \
    wallet_bumpfee.py \
    rpc_rawtransaction.py \
    rpc_blockchain.py \
    p2p_compactblocks.py \
    mining_basic.py \
    feature_block.py \
    "feature_segwit.py --legacy-wallet" \
    mempool_accept.py
fi

echo "=== functional tests: $* ==="
test/functional/test_runner.py --jobs="${FUNCTIONAL_JOBS:-1}" "$@"
SCRIPT
