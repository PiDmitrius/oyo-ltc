#!/usr/bin/env bash
#
# oyo.sh — portable installer / manager for an OYO stand.
#
# Ships inside the release archive next to node/ and web/. The directory that
# CONTAINS this script is the portable root; everything this stand writes lives
# under <root>/<network>/ — move the whole folder, re-run, and it follows.
#
#   ./oyo.sh full <network> [verb]                 litecoind + oyo-web here
#   ./oyo.sh node <network> [verb]                 only litecoind
#   ./oyo.sh web  <network> [verb] --rpc URL ...    only oyo-web -> an existing node
#
# Components install as systemd --user services (oyo-<net>-node / oyo-<net>-web),
# survive logout/reboot (enable-linger), and restart on failure. Secrets +
# configs are generated ONCE at first `up`; if the config already exists it is
# reused verbatim — re-running `up` never regenerates or overwrites anything.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_BIN="$ROOT/node/litecoind"
WEB_BIN="$ROOT/web/oyo-web"
UNIT_DIR="$HOME/.config/systemd/user"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

err()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }
sc()   { systemctl --user "$@"; }

usage() {
  cat <<'EOF'
OYO portable stand — litecoind node + oyo-web wallet/explorer.

Usage:
  ./oyo.sh full <network> [verb]                 node + oyo-web on this machine
  ./oyo.sh node <network> [verb]                 only litecoind
  ./oyo.sh web  <network> [verb] --rpc URL \
           [--rpc-user U] [--rpc-pass P]          only oyo-web -> an existing node

  <network> : mainnet | testnet | regtest
  verb      : up (default) | down | status | logs | update | restart
  options   : --web-port N   --web-user U   --web-pass P   --listen ADDR
              --rpc URL  --rpc-user U  --rpc-pass P   (web: the external node)

Examples:
  ./oyo.sh full mainnet
  ./oyo.sh web  mainnet --rpc http://127.0.0.1:9332 --rpc-user oyo --rpc-pass SECRET
  ./oyo.sh full regtest
  ./oyo.sh web  mainnet status
EOF
}

# ---------------------------------------------------------------- arg parsing
[ $# -ge 1 ] || { usage; exit 1; }
COMPONENT="$1"; shift
case "$COMPONENT" in
  full|node|web) ;;
  -h|--help) usage; exit 0 ;;
  *) err "unknown component '$COMPONENT' (expected full|node|web)" ;;
esac

RPC=""; RPC_USER=""; RPC_PASS=""
WEB_PORT=""; WEB_USER="oyo"; WEB_PASS=""; LISTEN=""
positional=()
while [ $# -gt 0 ]; do
  case "$1" in
    --rpc)       RPC="${2:?--rpc needs a value}";       shift 2 ;;
    --rpc-user)  RPC_USER="${2:?--rpc-user needs a value}"; shift 2 ;;
    --rpc-pass)  RPC_PASS="${2:?--rpc-pass needs a value}"; shift 2 ;;
    --web-port)  WEB_PORT="${2:?}";  shift 2 ;;
    --web-user)  WEB_USER="${2:?}";  shift 2 ;;
    --web-pass)  WEB_PASS="${2:?}";  shift 2 ;;
    --listen)    LISTEN="${2:?}";    shift 2 ;;
    -h|--help)   usage; exit 0 ;;
    --*)         err "unknown flag '$1'" ;;
    *)           positional+=("$1"); shift ;;
  esac
done
NETWORK="${positional[0]:-}"
VERB="${positional[1]:-up}"
[ -n "$NETWORK" ] || { usage; err "network required (mainnet|testnet|regtest)"; }
case "$NETWORK" in mainnet|testnet|regtest) ;; *) err "bad network '$NETWORK'";; esac
case "$VERB" in up|down|status|logs|update|restart) ;; *) err "bad verb '$VERB'";; esac

# ----------------------------------------------------------- per-network knobs
case "$NETWORK" in
  mainnet) NETCANON=main;    NODEFLAG="";         DEF_RPC_PORT=9332;  DEF_WEB_PORT=8891 ;;
  testnet) NETCANON=test;    NODEFLAG="-testnet"; DEF_RPC_PORT=19332; DEF_WEB_PORT=8892 ;;
  regtest) NETCANON=regtest; NODEFLAG="-regtest"; DEF_RPC_PORT=19443; DEF_WEB_PORT=8893 ;;
esac
[ -n "$WEB_PORT" ] || WEB_PORT="$DEF_WEB_PORT"
[ -n "$LISTEN" ]   || LISTEN="0.0.0.0:$WEB_PORT"

STAND="$ROOT/$NETWORK"
CONF="$STAND/litecoin.conf"
WEBCFG="$STAND/oyo-web.json"
NODE_SVC="oyo-$NETWORK-node"
WEB_SVC="oyo-$NETWORK-web"

gen_pass() { head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 32; }
need()     { command -v "$1" >/dev/null 2>&1 || err "missing required tool: $1"; }
json_get() { # json_get <file> <key>  — flat string field, no deps
  sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$1" | head -n1
}
conf_get() { sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*\(.*\)/\1/p" "$1" | head -n1; }

# ------------------------------------------------------ external node preflight
# (web component only) probe the node the operator pointed us at, fail early on
# the misconfigurations that otherwise surface as confusing runtime errors.
rpc_call() {
  curl -s --max-time 10 --user "$RPC_USER:$RPC_PASS" \
       --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"oyo\",\"method\":\"$1\",\"params\":${2:-[]}}" \
       -H 'content-type: text/plain' "$RPC"
}
preflight_node() {
  need curl
  [ -n "$RPC" ] || err "web component needs --rpc URL of the existing node"
  note "probing node $RPC"
  local info
  info="$(rpc_call getblockchaininfo)" || err "node unreachable at $RPC"
  echo "$info" | grep -q '"result"' || err "RPC error from node (auth/credentials?): $(echo "$info" | head -c 200)"
  local chain; chain="$(echo "$info" | grep -o '"chain":"[^"]*"' | cut -d'"' -f4 || true)"
  [ -n "$chain" ] || err "could not read chain from node"
  [ "$chain" = "$NETCANON" ] || err "node chain is '$chain' but you asked for '$NETWORK' ($NETCANON) — networks must match"
  echo "    ok: reachable, auth ok, chain=$chain"
  # txindex — oyo-web needs it for tx lookups / getrawtransaction
  local idx; idx="$(rpc_call getindexinfo 2>/dev/null || true)"
  if echo "$idx" | grep -q 'txindex'; then
    echo "$idx" | grep -q '"synced":true' && echo "    ok: txindex synced" || echo "    warn: txindex present but still syncing"
  else
    echo "    WARN: node does not report txindex — start litecoind with txindex=1 or tx pages will fail"
  fi
}

# --------------------------------------------------------------- config writers
write_litecoin_conf() { # args: rpcuser rpcpass rpcport
  mkdir -p "$STAND/chain"
  umask 077
  cat > "$CONF" <<EOF
# generated by oyo.sh — $NETWORK node. Edit + restart to change.
server=1
txindex=1
[$NETCANON]
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=$3
rpcuser=$1
rpcpassword=$2
EOF
}

write_web_json() { # args: rpcurl rpcuser rpcpass webpass
  mkdir -p "$STAND/mirror"
  umask 077
  cat > "$WEBCFG" <<EOF
{
  "listen": "$LISTEN",
  "rpc": "$1",
  "rpcuser": "$2",
  "rpcpassword": "$3",
  "network": "$NETCANON",
  "data_dir": "$STAND/mirror",
  "track_mempool": true,
  "webuser": "$WEB_USER",
  "webpassword": "$4",
  "native_block_parse": true
}
EOF
}

# ----------------------------------------------------------------- systemd units
write_node_unit() {
  mkdir -p "$UNIT_DIR"
  cat > "$UNIT_DIR/$NODE_SVC.service" <<EOF
[Unit]
Description=OYO $NETWORK litecoind node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$STAND
ExecStart=$NODE_BIN -datadir=$STAND/chain -conf=$CONF $NODEFLAG -printtoconsole
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
}
write_web_unit() { # arg: after-unit (node svc, or empty)
  mkdir -p "$UNIT_DIR"
  local after=""; [ -n "${1:-}" ] && after="After=$1.service
Requires=$1.service"
  cat > "$UNIT_DIR/$WEB_SVC.service" <<EOF
[Unit]
Description=OYO $NETWORK oyo-web (wallet + explorer)
$after

[Service]
Type=simple
WorkingDirectory=$STAND
ExecStart=$WEB_BIN -config $WEBCFG
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
}

ensure_linger() {
  if loginctl show-user "$USER" 2>/dev/null | grep -q 'Linger=yes'; then return 0; fi
  if loginctl enable-linger "$USER" 2>/dev/null; then
    note "enabled linger (stand survives logout / starts on boot)"
  else
    echo "    note: could not enable-linger automatically — run: sudo loginctl enable-linger $USER"
  fi
}

host_url() {
  local ip; ip="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
  [ -n "$ip" ] || ip="$(hostname 2>/dev/null || echo localhost)"
  echo "http://$ip:${LISTEN##*:}"
}

print_creds() {
  [ -f "$WEBCFG" ] || return 0
  local u p
  u="$(json_get "$WEBCFG" webuser)"; p="$(json_get "$WEBCFG" webpassword)"
  local rpc; rpc="$(json_get "$WEBCFG" rpc)"
  echo "────────────────────────────────────────────────────────"
  echo " OYO web · $NETWORK · node $rpc"
  echo "────────────────────────────────────────────────────────"
  echo "   URL:      $(host_url)   (listen $LISTEN)"
  echo "   Login:    $u / $p"
  echo "   Config:   $WEBCFG"
  echo "   Service:  $WEB_SVC ($(sc is-active "$WEB_SVC" 2>/dev/null || echo inactive))"
  echo "────────────────────────────────────────────────────────"
}

start_unit() { sc daemon-reload; sc enable --now "$1" >/dev/null 2>&1 || sc restart "$1"; }

# ----------------------------------------------------------------------- verbs
do_up() {
  need curl
  systemctl --user show-environment >/dev/null 2>&1 || err "systemd --user not available (need a real user session)"
  local want_node=0 want_web=0
  case "$COMPONENT" in
    full) want_node=1; want_web=1 ;;
    node) want_node=1 ;;
    web)  want_web=1 ;;
  esac

  local rpc_url rpc_user rpc_pass web_pass

  if [ "$COMPONENT" = web ]; then
    # external node: creds come from the operator; we only own the web side.
    [ -n "$RPC" ] || err "web needs --rpc URL [--rpc-user U --rpc-pass P]"
    preflight_node
    rpc_url="$RPC"; rpc_user="$RPC_USER"; rpc_pass="$RPC_PASS"
  else
    # full/node: we own the local node — generate (or reuse) its RPC creds.
    rpc_url="http://127.0.0.1:$DEF_RPC_PORT"
    if [ -f "$CONF" ]; then
      rpc_user="$(conf_get "$CONF" rpcuser)"; rpc_pass="$(conf_get "$CONF" rpcpassword)"
      note "$NETWORK/litecoin.conf exists → reusing it (not regenerating creds)"
    else
      rpc_user="oyo"; rpc_pass="$(gen_pass)"
      note "bootstrap: writing $NETWORK/litecoin.conf (txindex=1, generated rpc creds)"
      write_litecoin_conf "$rpc_user" "$rpc_pass" "$DEF_RPC_PORT"
    fi
  fi

  if [ "$want_web" = 1 ]; then
    if [ -f "$WEBCFG" ]; then
      note "$NETWORK/oyo-web.json exists → reusing it (not regenerating creds/config)"
      # honour an existing config verbatim; only flags that differ get a heads-up
      local cur_rpc; cur_rpc="$(json_get "$WEBCFG" rpc)"
      [ -n "$RPC" ] && [ "$RPC" != "$cur_rpc" ] && \
        echo "    note: --rpc '$RPC' ignored; config points at '$cur_rpc'. Edit $WEBCFG + restart to change."
      LISTEN="$(json_get "$WEBCFG" listen)"
    else
      [ -n "$WEB_PASS" ] || WEB_PASS="$(gen_pass)"
      note "bootstrap: writing $NETWORK/oyo-web.json (generated web login)"
      write_web_json "$rpc_url" "$rpc_user" "$rpc_pass" "$WEB_PASS"
    fi
  fi

  ensure_linger
  if [ "$want_node" = 1 ]; then write_node_unit; start_unit "$NODE_SVC"; note "$NODE_SVC: $(sc is-active "$NODE_SVC")"; fi
  if [ "$want_web"  = 1 ]; then
    [ "$COMPONENT" = full ] && write_web_unit "$NODE_SVC" || write_web_unit ""
    start_unit "$WEB_SVC"; note "$WEB_SVC: $(sc is-active "$WEB_SVC")"
  fi
  if [ "$want_node" = 1 ] && [ "$COMPONENT" != full ]; then
    echo "    node rpc: $rpc_url  user=$rpc_user  pass=$rpc_pass  (configure a remote web with these)"
  fi
  [ "$want_web" = 1 ] && print_creds || true
}

svcs_for_component() {
  case "$COMPONENT" in
    full) echo "$WEB_SVC $NODE_SVC" ;;   # web first (stop order)
    web)  echo "$WEB_SVC" ;;
    node) echo "$NODE_SVC" ;;
  esac
}

do_down()    { for s in $(svcs_for_component); do sc disable --now "$s" 2>/dev/null && note "stopped $s" || true; done
               echo "data kept under $STAND — remove it manually to wipe."; }
do_restart() { sc daemon-reload; for s in $(svcs_for_component); do sc restart "$s" && note "restarted $s"; done; }
do_update()  { note "re-applying units + restarting (binaries in node/ web/ are picked up)"
               [ "$COMPONENT" != web ] && write_node_unit || true
               if [ "$COMPONENT" != node ]; then [ "$COMPONENT" = full ] && write_web_unit "$NODE_SVC" || write_web_unit ""; fi
               do_restart; }
do_logs()    { local s; s="$(svcs_for_component | awk '{print $1}')"; sc status "$s" --no-pager -n 5 || true
               journalctl --user -u "$s" -f --no-pager; }
do_status()  {
  for s in $(svcs_for_component); do printf '   %-22s %s\n' "$s" "$(sc is-active "$s" 2>/dev/null || echo inactive)"; done
  if [ -f "$WEBCFG" ]; then
    print_creds
    local u p; u="$(json_get "$WEBCFG" webuser)"; p="$(json_get "$WEBCFG" webpassword)"
    local tip; tip="$(curl -s --max-time 5 --user "$u:$p" "http://127.0.0.1:${LISTEN##*:}/api/info" 2>/dev/null \
                      | grep -o '"blocks":[0-9]*' | head -n1 | cut -d: -f2 || true)"
    [ -n "$tip" ] && echo "   node tip: $tip" || true
  fi
}

[ -x "$NODE_BIN" ] || [ "$COMPONENT" = web ] || err "node binary missing: $NODE_BIN"
[ -x "$WEB_BIN" ]  || [ "$COMPONENT" = node ] || err "web binary missing: $WEB_BIN"

case "$VERB" in
  up)      do_up ;;
  down)    do_down ;;
  status)  do_status ;;
  logs)    do_logs ;;
  update)  do_update ;;
  restart) do_restart ;;
esac
