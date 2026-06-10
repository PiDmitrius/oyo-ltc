#!/bin/bash
set -e

# OYO LTC E2E Test Runner
# Uses litecoin-oyo Docker image for litecoind, builds oyo-web locally.
#
# Prerequisites:
#   - Docker image: litecoin-oyo (run docker/build-image.sh first)
#   - Go compiler
#   - Playwright (npx playwright)
#
# Usage: ./run-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OYO_PORT=18880
RPC_PORT=18543
CONTAINER_NAME="oyo-regtest-test-$$"

cleanup() {
    echo "Cleaning up..."
    [ -n "$OYO_PID" ] && kill "$OYO_PID" 2>/dev/null || true
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

echo "=== OYO LTC E2E Tests ==="

# 1. Start litecoind via Docker
echo "[1/4] Starting litecoind (regtest) in Docker..."
docker run -d --rm \
    --name "$CONTAINER_NAME" \
    -p "$RPC_PORT:19443" \
    litecoin-oyo -regtest

echo "   Waiting for RPC..."
for i in $(seq 1 30); do
    if curl -s --user oyo:oyo --data-binary \
        '{"jsonrpc":"1.0","id":"t","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' \
        "http://127.0.0.1:$RPC_PORT/" > /dev/null 2>&1; then
        echo "   litecoind ready."
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "ERROR: litecoind did not start in 30s"
        docker logs "$CONTAINER_NAME" 2>&1 | tail -20
        exit 1
    fi
    sleep 1
done

# 2. Build oyo-web
echo "[2/4] Building oyo-web..."
cd "$PROJECT_DIR"
go build -o /tmp/oyo-web-test-$$ .

# 3. Start oyo-web
echo "[3/4] Starting oyo-web..."
/tmp/oyo-web-test-$$ \
    -listen ":$OYO_PORT" \
    -rpc "http://127.0.0.1:$RPC_PORT" \
    -rpcuser oyo \
    -rpcpass oyo &
OYO_PID=$!

for i in $(seq 1 10); do
    if curl -s "http://127.0.0.1:$OYO_PORT/" > /dev/null 2>&1; then
        echo "   oyo-web ready (PID: $OYO_PID)."
        break
    fi
    if [ "$i" -eq 10 ]; then
        echo "ERROR: oyo-web did not start"
        exit 1
    fi
    sleep 1
done

# 4. Run Playwright tests
echo "[4/4] Running Playwright tests..."
echo ""
cd "$SCRIPT_DIR"
OYO_URL="http://127.0.0.1:$OYO_PORT" npx playwright test --reporter=list
EXIT_CODE=$?

# Cleanup oyo-web binary
rm -f /tmp/oyo-web-test-$$

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "=== ALL TESTS PASSED ==="
else
    echo "=== TESTS FAILED (exit $EXIT_CODE) ==="
fi

exit $EXIT_CODE
