#ifndef OYOLTC_H
#define OYOLTC_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #define OYO_API __declspec(dllexport)
#else
  #define OYO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define OYOLTC_ABI_VERSION 11u

typedef struct OYO_CTX_S*    OYO_CTX;
typedef struct OYO_CHAIN_S*  OYO_CHAIN;
typedef struct OYO_WALLET_S* OYO_WALLET;
typedef struct OYO_OP_S*     OYO_OP;

#define OYO_OK                     0
#define OYO_NO_CHANGE              1   /* positive: status ok, state unchanged since since_rev */
#define OYO_ERR_INVALID_ARG       -1
#define OYO_ERR_INVALID_HANDLE    -2
#define OYO_ERR_INVALID_JSON      -3
#define OYO_ERR_INVALID_STATE     -4
#define OYO_ERR_NOT_FOUND         -5
#define OYO_ERR_EXISTS            -6
#define OYO_ERR_UNSUPPORTED       -7
#define OYO_ERR_RPC               -8
#define OYO_ERR_REORG_TOO_DEEP    -9
#define OYO_ERR_INTERNAL         -99

#define OYO_OP_DONE      0
#define OYO_OP_NEED_RPC  1
#define OYO_OP_FAILED    2

/* Wallet kind — passed to oyo_wallet_open to pick the derivation chain
 * and which sides (canonical / MWEB) get pre-allocated. */
#define OYO_WALLET_KIND_REGULAR   1   /* P2WPKH only, oyo_v1 string seed                  */
#define OYO_WALLET_KIND_MWEB      2   /* MWEB stealth only, master from oyo_mweb_v1_seed  */
#define OYO_WALLET_KIND_UNIVERSAL 3   /* both sides, single seed string                   */
#define OYO_WALLET_KIND_WATCH     4   /* no derivation; addresses imported via add        */

/* Address-side selector for oyo_wallet_new_address. AUTO picks the
 * wallet's only side (regular→P2WPKH, mweb→MWEB); for universal the
 * caller must pass P2WPKH or MWEB explicitly. P2PKH (legacy) and
 * P2SH_P2WPKH (nested SegWit) are canonical-side alternatives derived
 * from the same oyo_v1 key — available on regular / universal wallets. */
#define OYO_ADDR_AUTO        0
#define OYO_ADDR_P2WPKH      1
#define OYO_ADDR_MWEB        2
#define OYO_ADDR_P2PKH       3
#define OYO_ADDR_P2SH_P2WPKH 4

OYO_API uint32_t oyo_version(void);

/* Opens a new context. `workdir_utf8` is an optional null-terminated UTF-8
 * path to a directory where per-chain sqlite mirrors live (one file per
 * network: oyoltc-{regtest,test,main}.db). Pass NULL or "" to run in
 * volatile mode — the MWEB mirror is opened as :memory: and discarded
 * on close. The directory is auto-created if missing. */
OYO_API int32_t oyo_open(const char* workdir_utf8, OYO_CTX* out);
OYO_API int32_t oyo_close(OYO_CTX ctx);

OYO_API int32_t oyo_last_error(
    OYO_CTX ctx,
    int32_t* out_code,
    const uint8_t** out_msg,
    size_t* out_msg_len);

OYO_API int32_t oyo_chain_open(
    OYO_CTX ctx,
    const uint8_t* cfg_json,
    size_t cfg_len,
    OYO_CHAIN* out);

OYO_API int32_t oyo_chain_close(OYO_CHAIN chain);

OYO_API int32_t oyo_chain_status(
    OYO_CHAIN chain,
    const uint8_t** out_json,
    size_t* out_len);

/* Address explorer over the local regular-UTXO mirror: confirmed balance +
 * unspent set for ANY canonical address (not just wallet-owned), with zero
 * node queries. Splits out the immature portion — coinbase outputs younger
 * than COINBASE_MATURITY (100) and peg-out (HogEx) vouts younger than
 * PEGOUT_MATURITY (6) — exactly as the node-wallet's getbalances does.
 *
 * Pending (mempool) IS included when the chain tracks the mempool
 * (track_mempool): pending_in_sat = unconfirmed receives, pending_out_sat =
 * this address's confirmed coins being spent in mempool — both from a
 * full-mempool per-script index, zero extra node queries. Zero when mempool
 * tracking is off.
 *
 * req_json selects the target — exactly one of:
 *   {"address":"<base58/bech32>"}   decoded against the chain's network
 *   {"script_hex":"<hex scriptPubKey>"}
 *
 * Result (borrowed chain-owned storage, valid until the next call on this
 * chain handle):
 *   {"address":"<canonical>", "script_hex":"...", "kind":"p2wpkh"|...,
 *    "confirmed_sat":<all unspent>, "available_sat":<confirmed - immature>,
 *    "immature_sat":<not-yet-matured coinbase/peg-out>,
 *    "pending_in_sat":<int>, "pending_out_sat":<int>, "pending_txids":[...],
 *    "utxo_count":<int>, "utxos_truncated":<bool>,
 *    "utxos":[{"txid","vout","amount_sat","height",
 *              "coinbase":<bool>,"pegout":<bool>,"mature":<bool>}, ...]}
 * Errors OYO_ERR_INVALID_ARG on an undecodable / non-canonical address
 * (e.g. an MWEB stealth address, which has no canonical scriptPubKey). */
OYO_API int32_t oyo_chain_address_status(
    OYO_CHAIN chain,
    const uint8_t* req_json,
    size_t req_len,
    const uint8_t** out_json,
    size_t* out_len);

OYO_API int32_t oyo_chain_sync(OYO_CHAIN chain, OYO_OP* out);

/* Drives a single mempool sync pass: getrawmempool, then getrawtransaction
 * for each new (unseen) txid in the diff. Tx-es that don't touch any
 * registered address are cached as "seen, not ours" to avoid refetching.
 * The same getrawtransaction(verbose).hex carries the MWEB extension under
 * rpcserialversion=2, so the MWEB pending diff (pending_in / pending_out) is
 * reconciled in the same pass — no node-side oyo-mweb-mempool RPC. The MWEB
 * counts ride under result.mweb. Returns OYO_OK with result.status ==
 * "disabled" if the chain was opened without "track_mempool":true. */
OYO_API int32_t oyo_mempool_sync(OYO_CHAIN chain, OYO_OP* out);

/* Wallet open. `kind` is one of OYO_WALLET_KIND_*. `wallet_json` carries
 * kind-specific config:
 *
 *   REGULAR    {"name", "seed", "address_count"?}
 *                seed: any free-form string. oyo_v1 chain.
 *   MWEB       {"name", "seed_type": "oyo_mweb_v1"|"mweb_v0", ...}
 *                oyo_mweb_v1: {"seed": "<any string>"} → master via SHA-256.
 *                mweb_v0:     {"scan_secret":"<hex64>", "spend_secret":"<hex64>"}.
 *   UNIVERSAL  {"name", "seed", "kinds"?, "address_count"?}
 *                seed: any string; canonical side via oyo_v1, MWEB side via
 *                oyo_mweb_v1_seed master. kinds optional cap to one side.
 *   WATCH      {"name"}
 *                Empty wallet; addresses added via oyo_wallet_add_address.
 */
OYO_API int32_t oyo_wallet_open(
    OYO_CHAIN chain,
    int32_t kind,
    const uint8_t* wallet_json,
    size_t wallet_len,
    OYO_WALLET* out);

OYO_API int32_t oyo_wallet_close(OYO_WALLET wallet);

/* Revision-aware status. Returns OYO_NO_CHANGE (1) if wallet.revision == since_rev;
 * in that case out_json/out_len are left untouched and out_rev is set to current. */
OYO_API int32_t oyo_wallet_revision(
    OYO_WALLET wallet,
    uint64_t* out_rev);

OYO_API int32_t oyo_wallet_status_since(
    OYO_WALLET wallet,
    uint64_t since_rev,
    uint64_t* out_rev,
    const uint8_t** out_json,
    size_t* out_len);

/* Export the wallet's secret material for backup / audit. Returns a JSON
 * blob (borrowed wallet-owned storage, valid until the next call on this
 * wallet):
 *   {"seed_type":"oyo_v1"|"oyo_mweb_v1"|"mweb_v0",
 *    "p2wpkh":[{"index":N,"address":"...","wif":"..."}],    // canonical side
 *    "mweb":{"scan_secret":"<hex32>","spend_secret":"<hex32>",
 *            "addresses":[{"index":N,"address":"..."}]}}     // mweb side
 * DANGEROUS — exposes spend authority; the caller MUST gate access.
 * Errors OYO_ERR_UNSUPPORTED for watch-only wallets (no secrets to export). */
OYO_API int32_t oyo_wallet_export_secrets(
    OYO_WALLET wallet,
    const uint8_t** out_json,
    size_t* out_len);

/* Allocate the next derived address. `addr_kind` selects the side:
 *   OYO_ADDR_AUTO   — wallet has only one side; that one
 *   OYO_ADDR_P2WPKH — canonical (regular/universal)
 *   OYO_ADDR_MWEB   — stealth (mweb/universal)
 * Errors with OYO_ERR_UNSUPPORTED if the requested side isn't enabled.
 * Watch wallets reject — use oyo_wallet_add_address.
 *
 * out_addr is borrowed wallet-owned storage; valid until the next call
 * on this wallet. */
OYO_API int32_t oyo_wallet_new_address(
    OYO_WALLET wallet,
    int32_t addr_kind,
    int32_t* out_index,
    const uint8_t** out_addr,
    size_t* out_addr_len);

/* Watch-only: import an externally-owned address. Decoded against the
 * chain's network (legacy / p2sh-segwit / bech32 P2WPKH / P2WSH).
 * Returns a stable binding index (slot in the wallet's binding vector).
 * Slot indices remain valid across removes; removed slots are left as
 * nullptr and not re-used. Errors with OYO_ERR_UNSUPPORTED on
 * non-watch wallets. */
OYO_API int32_t oyo_wallet_add_address(
    OYO_WALLET wallet,
    const uint8_t* address,
    size_t address_len,
    int32_t* out_binding_index);

OYO_API int32_t oyo_wallet_remove_address(
    OYO_WALLET wallet,
    int32_t binding_index);

OYO_API int32_t oyo_wallet_rescan(OYO_WALLET wallet, OYO_OP* out);

/* Rescan a single binding (by its derivation index within this wallet).
 * Affects the shared Address — other wallets watching the same script
 * will also see desync cleared and UTXO set refreshed. */
OYO_API int32_t oyo_wallet_rescan_address(
    OYO_WALLET wallet,
    int32_t binding_index,
    OYO_OP* out);

/* MWEB-side bootstrap: walks the local MWEB mirror (ForEachAll),
 * RewindOutput each output, AddMwebUtxo on matches. No-op for wallets
 * without an MWEB side — returns OYO_OK with a {"status":"skipped"} result. */
OYO_API int32_t oyo_wallet_bootstrap(
    OYO_WALLET wallet,
    OYO_OP* out);

/* Unified send. Internal dispatch picks the right finalizer based on
 * wallet kind + recipient class:
 *
 *   regular   + canonical  → P2WPKH→P2WPKH spend
 *   mweb      + stealth    → pure MWEB→MWEB
 *   mweb      + canonical  → MWEB peg-out
 *   universal + canonical  → P2WPKH→P2WPKH spend
 *   universal + stealth    → pure MWEB if mweb_balance >= amount+budget,
 *                             else peg-in (canonical inputs, MWEB recipient)
 *   regular   + stealth    → peg-in with canonical change
 *
 * config_json shape:
 *   {"to":"<address>",
 *    "amount_sat":<int>     | "send_all":true,
 *    "fee_rate_sat_per_vb":<int>?    // 0/absent → estimatesmartfee
 *    "dry_run":true?}
 *
 * Result on op completion:
 *   {"status":"sent"|"estimated", "txid":..., "amount_sat",
 *    "fee_sat" | ("canonical_fee_sat","mweb_fee_sat"),
 *    "change_sat" | ("canonical_change_sat","mweb_change_sat","change_on_mweb"),
 *    "inputs_total_sat", "inputs_count"} */
OYO_API int32_t oyo_wallet_send(
    OYO_WALLET wallet,
    const uint8_t* config_json,
    size_t config_len,
    OYO_OP* out);

OYO_API int32_t oyo_op_state(OYO_OP op, int32_t* out_state);

OYO_API int32_t oyo_op_rpc_request(
    OYO_OP op,
    const uint8_t** out_json,
    size_t* out_len);

OYO_API int32_t oyo_op_provide_rpc_response(
    OYO_OP op,
    const uint8_t* resp,
    size_t resp_len);

OYO_API int32_t oyo_op_result(
    OYO_OP op,
    const uint8_t** out_json,
    size_t* out_len);

OYO_API int32_t oyo_op_close(OYO_OP op);

/* MWEB self-test: verifies libmw symbols are linked and basic crypto math
 * (PublicKey::From, scalar mul, hash) works. Borrowed JSON output:
 *   {"status":"ok", "scan_pubkey":<hex33>, "spend_pubkey":<hex33>,
 *    "stealth_a":<hex33>, "stealth_b":<hex33>}
 * for a fixed 32-byte test seed (deterministic, no RNG). Used by Go cgo
 * tests to validate the MWEB build path. */
OYO_API int32_t oyo_mweb_self_test(
    OYO_CTX ctx,
    const uint8_t** out_json,
    size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif
