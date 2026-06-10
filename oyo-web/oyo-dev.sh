#!/bin/bash
set -e

# OYO LTC — build, test, deploy
# Usage: ./oyo.sh [quick|full]
# Default: quick

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_DIR="$SCRIPT_DIR/tests"

# Constants
CONTAINER=oyo-regtest
RUNTIME_IMAGE=litecoin-oyo

CONFIG_TEST="$SCRIPT_DIR/config-test.json"
CONFIG_USERTEST="$SCRIPT_DIR/config-usertest.json"

# Read values from test config
RPC_USER=$(python3 -c "import json;c=json.load(open('$CONFIG_TEST'));print(c.get('rpcuser',''))")
RPC_PASS=$(python3 -c "import json;c=json.load(open('$CONFIG_TEST'));print(c.get('rpcpassword',''))")
RPC_PORT=$(python3 -c "import json;c=json.load(open('$CONFIG_TEST'));print(c.get('rpc','').split(':')[-1].rstrip('/'))")
RPC_URL=$(python3 -c "import json;c=json.load(open('$CONFIG_TEST'));print(c.get('rpc',''))")
TEST_PORT=$(python3 -c "import json;c=json.load(open('$CONFIG_TEST'));print(c.get('listen','').rsplit(':',1)[-1])")
USERTEST_PORT=$(python3 -c "import json;c=json.load(open('$CONFIG_USERTEST'));print(c.get('listen','').rsplit(':',1)[-1])")

BINARY="$SCRIPT_DIR/oyo-web"
MODE="${1:-quick}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

step() { echo -e "\n${YELLOW}=== $1 ===${NC}"; }
ok()   { echo -e "${GREEN}✓ $1${NC}"; }
fail() { echo -e "${RED}✗ $1${NC}"; exit 1; }

cleanup_test() {
    kill "$TEST_PID" 2>/dev/null || true
}

wait_rpc() {
    local max=${1:-30}
    for i in $(seq 1 "$max"); do
        if curl -s --user "$RPC_USER:$RPC_PASS" \
            --data-binary '{"jsonrpc":"1.0","id":"t","method":"getblockchaininfo","params":[]}' \
            -H 'content-type: text/plain;' "$RPC_URL/" > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_http() {
    local port=$1 max=${2:-10}
    for i in $(seq 1 "$max"); do
        if curl -s "http://127.0.0.1:$port/" > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# ============================================================
# Full mode: rebuild litecoind + runtime image + node tests
# ============================================================
if [ "$MODE" = "full" ]; then
    step "1/F Building portable litecoind + liboyoltc cgo bundle (scripts/02 + scripts/04)"
    # scripts/02 builds the static portable litecoind (oyo-ltc-node-static
    # volume); scripts/04 builds the cgo bundle (liboyoltc_bundle.a) for
    # oyo-web in its own dynamic tree. Together they refresh BOTH the node
    # binary and the wallet-engine bundle.
    bash "$ROOT_DIR/scripts/02-build-litecoind.sh"
    bash "$ROOT_DIR/scripts/04-build-liboyoltc.sh"
    ok "litecoind compiled + cgo bundle rebuilt"

    step "2/F Building node runtime image (scripts/06)"
    bash "$ROOT_DIR/scripts/06-build-node-image.sh"
    ok "Runtime image rebuilt"

    step "3/F Running node tests (scripts/03: make check + functional smoke)"
    bash "$ROOT_DIR/scripts/03-test-litecoind.sh"
    ok "Node tests passed"

    step "4/F Restarting $CONTAINER"
    docker rm -f "$CONTAINER" 2>/dev/null || true
    docker run -d --name "$CONTAINER" -p "$RPC_PORT:19443" "$RUNTIME_IMAGE" -regtest -txindex
    ok "Container started"

    step "5/F Waiting for RPC"
    if ! wait_rpc 30; then
        docker logs "$CONTAINER" 2>&1 | tail -20
        fail "litecoind did not start in 30s"
    fi
    ok "RPC ready"
fi

# ============================================================
# Quick mode (also runs after full)
# ============================================================
BUNDLE="$ROOT_DIR/liboyoltc/lib/liboyoltc_bundle.a"

# liboyoltc auto-rebuild: oyo-dev.sh otherwise never rebuilds the cgo
# bundle, so an edit to liboyoltc/src/* (the wallet engine) would silently
# not reach the tests. If any liboyoltc source is newer than the bundle (or
# the bundle is missing), rebuild it here so `quick` is correct for ANY
# change, not just Go/frontend. After `full` this is a no-op — step 1/F
# already rebuilt the bundle. (Node changes in the litecoin-oyo-fork
# submodule still need `full` to rebuild litecoind and rebake the runtime
# image; that is not auto-detected here.)
if [ ! -f "$BUNDLE" ] || \
   [ -n "$(find "$ROOT_DIR/liboyoltc/src" \( -name '*.cpp' -o -name '*.h' \) -newer "$BUNDLE" 2>/dev/null | head -1)" ]; then
    step "0/Q liboyoltc changed (or no bundle) — rebuilding cgo bundle (scripts/04)"
    bash "$ROOT_DIR/scripts/04-build-liboyoltc.sh"
    ok "cgo bundle rebuilt"
fi

step "1/Q Building oyo-web (Go)"
cd "$SCRIPT_DIR"
# Force a clean relink when the cgo bundle is newer than the binary. `go
# build` does NOT hash external -l archives, so an internal-only liboyoltc
# change (no .go/.h/ABI diff) would silently reuse a stale binary. Clearing
# the build cache + removing the output reliably pulls the fresh bundle in.
if [ "$BUNDLE" -nt "$BINARY" ]; then
    echo "  cgo bundle newer than binary — forcing clean relink"
    go clean -cache
    rm -f "$BINARY"
fi
go build -o "$BINARY" .
ok "Binary built"

step "1.5/Q Running Go tests"
go test ./...
ok "Go tests passed"

step "2/Q Fresh regtest"
docker rm -f "$CONTAINER" 2>/dev/null || true
# Test server data lives in its own dir (data_dir in config-test.json).
# Wipe it on every run so the test session starts with no stale chain
# state — independent from the usertest server's data dir which keeps
# running uninterrupted at $SCRIPT_DIR (oyoltc-*.db there belongs to
# the systemctl --user oyo-web instance, not the test).
rm -rf "$SCRIPT_DIR/test-data" "$SCRIPT_DIR/queue" 2>/dev/null || true
docker run -d --name "$CONTAINER" -p "$RPC_PORT:19443" "$RUNTIME_IMAGE" -regtest -txindex
if ! wait_rpc 30; then
    docker logs "$CONTAINER" 2>&1 | tail -20
    fail "litecoind did not start in 30s"
fi
ok "Clean regtest started"

# Ensure MWEB is activated (needs 432 blocks in regtest)
HEIGHT=$(curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
    '{"jsonrpc":"1.0","id":"h","method":"getblockchaininfo","params":[]}' \
    -H 'content-type: text/plain;' "$RPC_URL/" 2>/dev/null | \
    python3 -c "import json,sys;print(json.load(sys.stdin)['result']['blocks'])" 2>/dev/null || echo 0)
# Ensure MWEB is activated (needs height >= 432 in regtest)
# Mine in two phases: pre-MWEB (431) then post-activation (remaining)
ensure_mweb() {
    local height
    height=$(curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        '{"jsonrpc":"1.0","id":"h","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' "$RPC_URL/" 2>/dev/null | \
        python3 -c "import json,sys;print(json.load(sys.stdin)['result']['blocks'])" 2>/dev/null || echo 0)
    [ "$height" -ge 432 ] && return 0

    step "  Mining to MWEB activation (height $height → 432+)"
    # Ensure wallet for mining address
    curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        '{"jsonrpc":"1.0","id":"w","method":"createwallet","params":["regtest-miner"]}' \
        -H 'content-type: text/plain;' "$RPC_URL/" > /dev/null 2>&1
    local addr
    addr=$(curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        '{"jsonrpc":"1.0","id":"a","method":"getnewaddress","params":[]}' \
        -H 'content-type: text/plain;' "$RPC_URL/wallet/regtest-miner" 2>/dev/null | \
        python3 -c "import json,sys;print(json.load(sys.stdin)['result'])" 2>/dev/null)

    # Phase 1: mine to 431 (pre-MWEB activation)
    if [ "$height" -lt 431 ]; then
        local pre=$((431 - height))
        curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
            "{\"jsonrpc\":\"1.0\",\"id\":\"m\",\"method\":\"generatetoaddress\",\"params\":[$pre, \"$addr\"]}" \
            -H 'content-type: text/plain;' "$RPC_URL/" > /dev/null 2>&1
    fi

    # Phase 2: peg-in tx + mine block 432 (MWEB activation requires a peg-in in the first MWEB block)
    local mweb_addr
    mweb_addr=$(curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        '{"jsonrpc":"1.0","id":"a","method":"getnewaddress","params":["", "mweb"]}' \
        -H 'content-type: text/plain;' "$RPC_URL/wallet/regtest-miner" 2>/dev/null | \
        python3 -c "import json,sys;print(json.load(sys.stdin)['result'])" 2>/dev/null)
    curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        "{\"jsonrpc\":\"1.0\",\"id\":\"s\",\"method\":\"sendtoaddress\",\"params\":[\"$mweb_addr\", 1]}" \
        -H 'content-type: text/plain;' "$RPC_URL/wallet/regtest-miner" > /dev/null 2>&1
    curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        "{\"jsonrpc\":\"1.0\",\"id\":\"m\",\"method\":\"generatetoaddress\",\"params\":[1, \"$addr\"]}" \
        -H 'content-type: text/plain;' "$RPC_URL/" > /dev/null 2>&1

    height=$(curl -s --user "$RPC_USER:$RPC_PASS" --data-binary \
        '{"jsonrpc":"1.0","id":"h","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' "$RPC_URL/" 2>/dev/null | \
        python3 -c "import json,sys;print(json.load(sys.stdin)['result']['blocks'])" 2>/dev/null || echo 0)

    if [ "$height" -ge 432 ]; then
        ok "MWEB activated (height $height)"
    else
        fail "MWEB activation failed (height $height)"
    fi
}
ensure_mweb

step "3/Q Starting test server (:$TEST_PORT)"
trap cleanup_test EXIT
"$BINARY" -config "$CONFIG_TEST" &
TEST_PID=$!
if ! wait_http "$TEST_PORT" 10; then
    fail "Test server did not start"
fi
ok "Test server running (PID $TEST_PID)"

step "3.1/Q Waiting for engine to catch up to node tip (walk-from-genesis)"
# engine=mirror self-seeds by walking from genesis on first start, so the
# wallet engine is not fully populated until the mirror reaches the node tip.
# Drive chain/sync (each call walks up to kMaxBlocksPerSyncCall=100 blocks)
# until it reports caught up, before any faucet/wallet work.
SYNC_URL="http://127.0.0.1:$TEST_PORT/api/chain/sync"
caught_up=0
for i in $(seq 1 120); do
    if curl -s -X POST "$SYNC_URL" 2>/dev/null | grep -q synced; then
        caught_up=1
        ok "Engine caught up to node tip"
        break
    fi
    sleep 0.5
done
[ "$caught_up" = "1" ] || fail "Engine did not catch up to node tip"

step "3.5/Q Pre-funding faucets (test-oyo-e2e + test-e2e)"
# Two faucets, both pre-funded by direct coinbase mining so tests
# never need to mine more than a couple of blocks for confirmation:
#
#   • test-oyo-e2e (OYO regular HD, canonical-only side):
#     Used by the matrix to fund fresh OYO wallets. Sends never route
#     through peg-out — no HogEx maturity races on recipient vouts.
#
#   • test-e2e (node HD wallet):
#     Legacy faucet kept for parity / shadow / mempool-tracking tests
#     that compare a node-wallet sender to an OYO recipient.
#
# Both get coinbase rewards aimed at their first bech32 binding; +100
# blocks past the deepest coinbase clears Litecoin Core's coinbase-
# maturity gate so the wallets can spend the rewards as ordinary UTXO.
FAUCET_URL="http://127.0.0.1:$TEST_PORT/api"

curl -s -X POST "$FAUCET_URL/wallet/create?name=test-oyo-e2e&type=regular&seed=test-oyo-e2e-funding-seed-v1&address_count=8" >/dev/null
curl -s -X POST "$FAUCET_URL/wallet/create?name=test-e2e&type=seed&seed=test-e2e-faucet-seed-v1" >/dev/null
OYO_ADDR=$(curl -s "$FAUCET_URL/wallet/info?name=test-oyo-e2e" | \
    python3 -c "import json,sys;print(json.load(sys.stdin)['addresses'][0]['address'])" 2>/dev/null)
E2E_ADDR=$(curl -s -X POST "$FAUCET_URL/wallet/newaddress?name=test-e2e&type=bech32" | \
    python3 -c "import json,sys;print(json.load(sys.stdin)['address'])" 2>/dev/null)
if [ -z "$OYO_ADDR" ] || [ -z "$E2E_ADDR" ]; then
    fail "Could not derive faucet bech32 addresses (oyo=$OYO_ADDR e2e=$E2E_ADDR)"
fi

# Coinbase rewards at the regtest subsidy near MWEB activation
# (~12.5 LTC pre-halving). 30 coinbases for the OYO funder + 60 for
# test-e2e gives both faucets ~200–400 LTC of canonical UTXO. Tests
# fund fresh wallets in 0.1–2 LTC chunks plus a few peg-in heavy tests
# that spend ~5–10 LTC each — total e2e drain across the whole suite
# is ~150 LTC, the headroom keeps trusted balance comfortably above
# the 5 LTC asserts. +100 blocks past the deepest faucet coinbase
# clears Litecoin Core's coinbase-maturity gate.
curl -s -X POST "$FAUCET_URL/mine?count=30&address=$OYO_ADDR" >/dev/null
curl -s -X POST "$FAUCET_URL/mine?count=60&address=$E2E_ADDR" >/dev/null
curl -s -X POST "$FAUCET_URL/mine?count=100" >/dev/null
curl -s -X POST "$FAUCET_URL/chain/sync" >/dev/null

OYO_BAL=$(curl -s "$FAUCET_URL/wallet/info?name=test-oyo-e2e" | \
    python3 -c "import json,sys;print(json.load(sys.stdin).get('confirmed_sat',0))" 2>/dev/null)
E2E_BAL=$(curl -s "$FAUCET_URL/wallet/balances?name=test-e2e" | \
    python3 -c "import json,sys;print(int(round(json.load(sys.stdin).get('mine',{}).get('trusted',0)*1e8)))" 2>/dev/null)
ok "Faucets ready — test-oyo-e2e=$OYO_BAL sat, test-e2e=$E2E_BAL sat"

step "4/Q Running Playwright tests"
cd "$TESTS_DIR"
if OYO_URL="http://127.0.0.1:$TEST_PORT" npx playwright test --reporter=list; then
    ok "All tests passed"
else
    kill "$TEST_PID" 2>/dev/null || true
    fail "Tests failed — usertest not updated"
fi

step "5/Q Stopping test server"
kill "$TEST_PID" 2>/dev/null || true
wait "$TEST_PID" 2>/dev/null || true
ok "Test server stopped"

if [ -n "$OYO_SKIP_DEPLOY" ]; then
    echo -e "\n${GREEN}=== DONE ($MODE, deploy skipped: OYO_SKIP_DEPLOY) ===${NC}"
    exit 0
fi

step "6/Q Deploying to usertest (:$USERTEST_PORT)"
# oyo-web ships as a per-user systemd unit (no sudo required for
# restart). User lingering keeps the service alive after logout.
if systemctl --user list-unit-files oyo-web.service --no-legend 2>/dev/null | grep -q '^oyo-web.service'; then
    systemctl --user restart oyo-web
    if ! wait_http "$USERTEST_PORT" 10; then
        fail "Usertest systemd --user service did not start"
    fi
    ok "Usertest running via systemctl --user"
    trap - EXIT
    echo -e "\n${GREEN}=== DONE ($MODE) ===${NC}"
    exit 0
fi
# Kill existing usertest by port and wait for it to free
USERTEST_PID_OLD=$(ss -tlnp "sport = :$USERTEST_PORT" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)
[ -n "$USERTEST_PID_OLD" ] && kill "$USERTEST_PID_OLD" 2>/dev/null || true
for i in $(seq 1 20); do
    ss -tlnp | grep -q ":$USERTEST_PORT " || break
    sleep 0.2
done
if ss -tlnp | grep -q ":$USERTEST_PORT "; then
    fail "Port $USERTEST_PORT still in use after 4s"
fi
cd "$SCRIPT_DIR"
"$BINARY" -config "$CONFIG_USERTEST" &
USERTEST_PID=$!
disown "$USERTEST_PID"
if ! wait_http "$USERTEST_PORT" 10; then
    fail "Usertest server did not start"
fi
ok "Usertest running (PID $USERTEST_PID)"

trap - EXIT
echo -e "\n${GREEN}=== DONE ($MODE) ===${NC}"
