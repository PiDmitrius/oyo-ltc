#include "oyoltc.h"
#include "mweb.h"

#include <bech32.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <random.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <uint256.h>
#include <core_io.h>            // #5: TxToUniv + DecodeHexBlk for native block parse
#include <primitives/block.h>   // #5: CBlock + MWEB::Block mweb_block
#include <mw/models/block/Block.h> // #5: mw::Block GetInputs/GetOutputs (+ Input/Output)
#include <univalue.h>
#include <util/rbf.h>
#include <util/strencodings.h>

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

// All internal types and helpers live in `oyoltc::detail` so that
// MwebAddress can be forward-declared from mweb.h (anonymous-namespace
// types have internal linkage and cannot be referenced cross-TU).
namespace oyoltc { namespace detail {

constexpr uint32_t kMagicCtx     = 0x4F594354u; // 'OYCT'
constexpr uint32_t kMagicChain   = 0x4F594348u; // 'OYCH'
constexpr uint32_t kMagicWallet  = 0x4F595741u; // 'OYWA'
constexpr uint32_t kMagicOp      = 0x4F59504Fu; // 'OYPO'
constexpr uint32_t kMagicDead    = 0xDEADBEEFu;

constexpr int64_t kCoin                 = 100000000;
constexpr size_t  kDefaultRollbackWin   = 100;
constexpr size_t  kMaxRollbackWin       = 1000;
constexpr int64_t kDefaultMaxReorgDepth = 5000;
constexpr int     kMaxBlocksPerSyncCall = 100;
constexpr int     kBootstrapBatchBlocks = 100;   // mirror commits per batch during the bootstrap walk
constexpr size_t  kPruneMaxDeletesPerCall = 20000; // per-block cap on reorg-journal prune deletes (bounds the WriteBatch)
constexpr size_t  kMaxUtxosListed       = 2000;  // cap on per-address utxo / history arrays serialised to JSON (MWEB arm)
// Explorer regular-address scan window. The walk over a script's outputs is
// newest-first and costs ~one random spent-index Get per output, so a wide
// window is slow on cold cache / slow disk (≈5 ms/output measured). Default
// small so the lock is held only briefly; a caller widens it via ?limit with
// NO ceiling — the UI's doubling "load more" goes as deep as the operator wants
// (they accept the latency). kAddrListMax only bounds the materialised JSON
// arrays so a pathological ?limit cannot OOM the host; the scan + running sums
// (balance) keep going past it.
constexpr size_t  kAddrScanDefault      = 256;
constexpr size_t  kAddrListMax          = 100000;
// Mirrors PEGOUT_MATURITY in src/consensus/consensus.h. Number of blocks
// a peg-out (HogEx vout > 0) must be buried before it can be spent.
constexpr int64_t kPegoutMaturity       = 6;
// Mirrors COINBASE_MATURITY in src/consensus/consensus.h. Coinbase outputs
// (block reward) need this many confirmations before they can be spent;
// used only to split the immature portion out of an address's balance.
constexpr int64_t kCoinbaseMaturity     = 100;

struct OyoCtxImpl;
struct OyoChainImpl;
struct OyoWalletImpl;
struct OyoOpImpl;
class  MwebMirror;
class  RegularMirror;
class  BlockTrail;

int64_t NowMicros() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count();
}

struct TimingStat {
    int64_t count = 0;
    int64_t total_us = 0;
    int64_t max_us = 0;
};

void TimingAdd(TimingStat& s, int64_t elapsed_us) {
    if (elapsed_us < 0) elapsed_us = 0;
    s.count++;
    s.total_us += elapsed_us;
    if (elapsed_us > s.max_us) s.max_us = elapsed_us;
}

void TimingAddSince(TimingStat& s, int64_t start_us) {
    TimingAdd(s, NowMicros() - start_us);
}

UniValue TimingJson(const TimingStat& s) {
    UniValue v(UniValue::VOBJ);
    v.pushKV("count", s.count);
    v.pushKV("total_ms", s.total_us / 1000);
    v.pushKV("avg_us", s.count ? (s.total_us / s.count) : int64_t(0));
    v.pushKV("max_us", s.max_us);
    return v;
}

struct SyncPerfStats {
    int64_t blocks = 0;
    int64_t last_block_height = -1;
    int64_t last_block_us = 0;
    TimingStat native_decode;
    TimingStat apply_block;
    TimingStat parse_regular_events;
    TimingStat apply_regular_events;
    TimingStat parse_mweb_events;
    TimingStat apply_mweb_events;
    TimingStat mirror_mweb_feed;
    TimingStat mirror_regular_feed;
    TimingStat mirror_commit;
};

// Bridge Litecoin ECC lifecycle to ctx lifetime via refcount.
// ECC_Start initializes the SIGN context (used by CKey signing); the
// VERIFY context (CheckLowS in pubkey.cpp, ProduceSignature flow) needs
// a separate ECCVerifyHandle. Order mirrors init.cpp:1244-1245.
struct ScopedEcc {
    std::unique_ptr<ECCVerifyHandle> verify_handle;
    ScopedEcc() {
        ECC_Start();
        verify_handle.reset(new ECCVerifyHandle());
    }
    ~ScopedEcc() {
        verify_handle.reset();
        ECC_Stop();
    }
};
struct Globals {
    std::mutex mu;
    std::unique_ptr<ScopedEcc> ecc;
    int refs = 0;
    void acquire() {
        std::lock_guard<std::mutex> g(mu);
        if (refs == 0) ecc = std::make_unique<ScopedEcc>();
        refs++;
    }
    void release() {
        std::lock_guard<std::mutex> g(mu);
        if (--refs == 0) ecc.reset();
    }
};
Globals& G() { static Globals g; return g; }

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct UtxoKey {
    std::string txid;
    uint32_t    vout;
    bool operator==(const UtxoKey& o) const { return txid == o.txid && vout == o.vout; }
};
struct UtxoKeyHash {
    size_t operator()(const UtxoKey& k) const {
        size_t h = std::hash<std::string>{}(k.txid);
        h ^= std::hash<uint32_t>{}(k.vout) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct Utxo {
    std::string txid;
    uint32_t    vout                 = 0;
    int64_t     amount_sat           = 0;
    int64_t     height               = 0;       // 0 if !confirmed
    bool        confirmed            = false;   // false = lives in mempool only
    bool        spent                = false;   // confirmed-spent (in a block)
    int64_t     spent_height         = 0;
    bool        spent_pending        = false;   // spent by a mempool tx, not yet in a block
    std::string spent_pending_txid;              // which mempool tx spends this utxo
    // True for vouts of the block's HogEx tx (peg-out materialisation):
    // the node enforces PEGOUT_MATURITY (6) blocks before such vouts can
    // be spent. Coin selection filters these until matured.
    bool        is_pegout_output     = false;
    // True for vouts of the block's coinbase tx: the node enforces
    // COINBASE_MATURITY (100) blocks before they can be spent.
    bool        is_coinbase          = false;
};

// Returns true when a confirmed UTXO is still inside its maturity window
// (peg-out or coinbase). Node consensus checks `nSpendHeight - coin.nHeight
// >= MATURITY` (consensus/tx_verify.cpp); mempool acceptance uses
// nSpendHeight = tip+1 (the next block being assembled), so the predicate is
// `(tip+1) - u.height >= M` <=> `(tip - u.height) >= M - 1`. Without a tip
// yet (cold start) UTXOs are treated as mature.
bool IsImmatureUtxoLocked(const Utxo& u, int64_t tip_height) {
    if (tip_height < 0) return false;
    if (u.is_pegout_output && (tip_height - u.height) < (kPegoutMaturity - 1)) return true;
    if (u.is_coinbase && (tip_height - u.height) < (kCoinbaseMaturity - 1)) return true;
    return false;
}

// Returns true when a UTXO is currently spendable. Filters out
// unconfirmed / spent / pending-spent UTXOs and confirmed ones still
// inside a maturity window. Reported immature_sat uses the same
// predicate, so it always equals what coin selection filters out.
bool IsSpendableUtxoLocked(const Utxo& u, int64_t tip_height) {
    if (u.spent || u.spent_pending || !u.confirmed) return false;
    return !IsImmatureUtxoLocked(u, tip_height);
}

struct Address {
    std::string kind;         // "p2wpkh"
    std::string script_hex;   // "0014<h160>"
    std::string address;      // bech32
    std::unordered_map<UtxoKey, Utxo, UtxoKeyHash> utxos; // includes spent + pending
    int64_t     confirmed_sat   = 0;   // confirmed && !spent && !spent_pending
    int64_t     pending_in_sat  = 0;   // !confirmed && !spent && !spent_pending
    int64_t     pending_out_sat = 0;   // confirmed && !spent &&  spent_pending
    bool        desync          = true;
    std::vector<OyoWalletImpl*> watchers;
};

enum class EventKind : int8_t { ADD = 0, SPEND = 1 };

struct BlockEvent {
    EventKind   kind;
    UtxoKey     key;          // {txid, vout}; for ADD: this tx; for SPEND: prev
    std::string script_hex;   // for ADD: from vout; for SPEND: computed from witness
    int64_t     amount_sat;   // for ADD: from vout; for SPEND: 0 (taken from addr.utxos)
    std::string event_tx;     // txid of THIS tx — for ADD == key.txid, for SPEND = spender
    bool        is_pegout_output = false;  // ADD-only: HogEx vout that needs PEGOUT_MATURITY blocks before spend
    bool        is_coinbase      = false;  // ADD-only: coinbase vout that needs COINBASE_MATURITY blocks before spend
};

struct Block {
    int64_t                 height;
    std::string             hash;
    std::vector<BlockEvent> events;
};

struct PendingTx {
    std::string             txid;
    std::vector<BlockEvent> events;     // same shape as block events
    int64_t                 first_seen = 0;
    bool                    promoted   = false; // block already applied this tx; await mempool diff
};

// --- engine-side pending for ANY address (transient-watcher) ---------------
// A lightweight per-tx view of the mempool: each tx's outputs (script, amount,
// vout) and inputs (prevout). pending_in/out for an arbitrary address is derived
// ON DEMAND in oyo_chain_address_status by scanning these against the address's
// UTXO set — no global per-script aggregate, no outpoint->owner resolution (so
// no idx_output_outpoint). Orthogonal to the watched-Address PendingTx machinery
// above (which drives wallet balances + coin selection); this serves read-only
// address queries only. Empty when track_mempool is off.
struct MempoolTxView {
    struct Out { uint32_t vout; std::string script_hex; int64_t amount; };
    struct In  { std::string ptxid; uint32_t pvout; };
    std::string      txid;
    std::vector<Out> outs;
    std::vector<In>  ins;
};

struct Binding {
    Address* addr;  // non-owning; owned by chain.addresses
    int      index; // stable id within this wallet (vector slot)
};

// ---------------------------------------------------------------------------
// MWEB types — parallel to P2WPKH (Address/Utxo/Binding/BlockEvent), keyed by
// commitment instead of script_hex. Just like canonical addresses, MwebAddress
// is a chain-level entity: identity = bech32 stealth address. Two wallets that
// share a seed derive the same (scan_pubkey, spend_pubkey) per index → same
// MwebAddress, with both wallets registered as watchers. Per-wallet bindings
// (MwebBinding) carry only the slot index; the address state itself is shared.
// ---------------------------------------------------------------------------

// Hash adapter for std::array<u8, N> map keys — defined in mweb.h
// (oyoltc::ArrayHash33 / ArrayHash32) and used here too. Bring into
// the detail namespace via aliases for brevity.
using ::oyoltc::ArrayHash32;
using ::oyoltc::ArrayHash33;

struct MwebUtxo {
    std::array<uint8_t, 33> commitment{};     // serialized Pedersen commitment
    std::array<uint8_t, 32> output_id{};      // mw::Hash
    std::array<uint8_t, 32> shared_secret{};  // `t` from RewindOutput; needed to spend
    int64_t     amount_sat   = 0;
    int64_t     height       = 0;
    bool        confirmed    = false;
    bool        spent        = false;
    int64_t     spent_height = 0;
};

struct MwebAddress {
    uint32_t    address_index = 0;     // subderivation index (intrinsic to the address)
    std::string mweb_address;          // bech32 stealth address — identity (string)
    std::array<uint8_t, 33> scan_pubkey{};   // A_i = B_i * scan_secret
    std::array<uint8_t, 33> spend_pubkey{};  // B_i
    std::unordered_map<std::array<uint8_t, 33>, MwebUtxo, ArrayHash33> utxos; // by commitment
    int64_t     confirmed_sat   = 0;
    int64_t     pending_in_sat  = 0;   // sum of pending mempool MWEB outputs
    int64_t     pending_out_sat = 0;   // sum of confirmed UTXOs being spent in mempool
    bool        desync          = false; // matches Address.desync semantics
    std::vector<OyoWalletImpl*> watchers; // wallets with a binding to this address
};

struct MwebBinding {
    MwebAddress* addr;  // non-owning; owned by chain.mweb_addresses
    uint32_t     index; // == addr->address_index for current append-only allocate
};

// Master keychain registered with the chain. Dedup'd across wallets that
// share the same (scan_secret, spend_pubkey) — two wallets opened with the
// same seed re-use a single keychain entry. The map's value is a chain-
// level MwebAddress*, so TryRewindOutput resolves identity AND ownership
// in one lookup (no separate idx → address indirection).
struct MwebKeychain {
    std::array<uint8_t, 32> scan_secret;
    std::array<uint8_t, 33> spend_pubkey;  // master B (compressed)
    ::oyoltc::mweb::SpendPubkeyMap spend_pubkey_index;  // B_i → MwebAddress*
};

// Chain-side index entry for an active MWEB UTXO. Holds a non-owning pointer
// to the chain-level MwebAddress so Spend events resolve to a single shared
// address state, regardless of how many wallets watch it.
struct MwebUtxoEntry {
    MwebAddress*   owner_addr = nullptr;
    int64_t        amount_sat   = 0;
    int64_t        height       = 0;
    std::array<uint8_t, 32> output_id{};
    std::array<uint8_t, 32> shared_secret{};
};

enum class MwebEventKind : int8_t { ADD = 0, SPEND = 1 };

struct MwebBlockEvent {
    MwebEventKind kind;
    std::array<uint8_t, 33> commitment{};        // identity for both ADD and SPEND
    std::array<uint8_t, 32> output_id{};         // for ADD; zeros for SPEND
    std::array<uint8_t, 32> shared_secret{};     // for ADD; zeros for SPEND
    int64_t     amount_sat   = 0;                // for ADD; 0 for SPEND
    MwebAddress* owner_addr = nullptr;
};

enum MirrorMetaKey : int64_t {
    kMetaState            = 1,
    kMetaRegularOutputs   = 2,
    kMetaRegularSpends    = 3,
    kMetaMwebOutputs      = 4,
    kMetaMwebSpends       = 5,
    kMetaSchemaVersion    = 6,  // on-disk format version; open rejects a mismatch (wipe required)
    kMetaRegularPruneFloor = 7, // height below which o/s reorg journals are fully pruned
};

// Bumped whenever the key/value layout changes incompatibly. v3 = ASCII
// prefixes + hash20 keys + u32 block height (Stage 1+2 redesign).
constexpr int64_t kMirrorSchemaVersion = 3;

enum MirrorState : int64_t {
    kMirrorBootstrap = 0,
    kMirrorIndexing  = 1,
    kMirrorWorking   = 2,
};

struct MirrorDelta {
    int64_t regular_outputs = 0;
    int64_t regular_spends  = 0;
    int64_t mweb_outputs    = 0;
    int64_t mweb_spends     = 0;

    void Clear() {
        regular_outputs = regular_spends = mweb_outputs = mweb_spends = 0;
    }
};

struct OyoWalletImpl {
    uint32_t        magic = kMagicWallet;
    OyoChainImpl*   chain = nullptr;
    std::string     name;
    std::string     seed_type;                 // "oyo_v1" | "mweb_v0" | "oyo_mweb_v1"
    std::vector<uint8_t> seed_bytes;
    int             address_count = 8;
    // Earliest block height this wallet could hold funds from. 0 = scan from
    // genesis. Used to skip the pre-birth prefix of the MWEB journal during
    // local bootstrap (stealth has no per-address on-chain index, so the bulk
    // RewindOutput scan is the slow path — a birth height cuts it down a lot).
    // Caller's responsibility to set it no later than the first real receive.
    int64_t         birth_height = 0;
    std::string     address_kind;              // "p2wpkh" | "mweb"
    std::vector<std::unique_ptr<Binding>> bindings; // null entry = removed slot, index stays stable
    bool            watch_only  = false;
    int64_t         balance_sat = 0;           // cached sum of bindings.addr->confirmed_sat
    uint64_t        revision    = 0;           // monotonic; bumps on any state delta
    std::string     status_cache;              // rendered on demand in wallet_status_since
    uint64_t        status_cache_rev = 0;      // revision at which status_cache was built
    int64_t         status_cache_tip = -2;     // chain.tip_height at last status build —
                                               // immature_sat depends on tip - utxo.height,
                                               // so cache invalidates when tip advances
                                               // even if revision didn't bump.
    std::string     secrets_cache;             // backing store for the borrowed
                                               // JSON returned by oyo_wallet_export_secrets.

    // MWEB-only fields. wallet_kind discriminates between p2wpkh-family
    // (regular/watch) and mweb-family. For an mweb wallet, mweb_keychain is
    // non-null and points to a (dedup'd) entry in chain.mweb_keychains;
    // mweb_bindings holds per-slot MwebBinding entries pointing to chain-
    // level MwebAddress objects shared with any other watcher wallets.
    std::string                                wallet_kind;     // "node" | "mweb" | "universal"
    // Capability flags. A wallet can have either or both sides enabled
    // (universal). Capped wallets reject sends/recvs on the disabled side
    // with a clear message. has_p2wpkh implies bindings[] is the storage;
    // has_mweb implies mweb_keychain + mweb_bindings[] are storage.
    bool                                       has_p2wpkh = false;
    bool                                       has_mweb   = false;
    MwebKeychain*                              mweb_keychain  = nullptr;
    std::vector<std::unique_ptr<MwebBinding>>  mweb_bindings;   // by address_index
    // Cached confirmed-MWEB total — symmetric to canonical balance_sat.
    // Updated by NotifyMwebWatchersLocked. Pending-in/out are NOT cached;
    // status JSON sums them on demand from mweb_bindings (canonical does
    // the same for canonical pending in oyoltc.cpp:2255-2256).
    int64_t                                    mweb_balance_sat     = 0;
};

struct OyoChainImpl {
    uint32_t        magic = kMagicChain;
    OyoCtxImpl*     ctx = nullptr;
    std::string     network;                   // "main" | "test" | "regtest"
    std::string     hrp;                       // "ltc" | "tltc" | "rltc"
    int64_t         tip_height = -1;
    // Node's chain height as of the last getblockchaininfo. Lets the bootstrap
    // walk skip writing reorg journals for blocks already buried deeper than
    // max_reorg_depth (final, never reorged). -1 until the first sync poll.
    int64_t         node_tip_hint = -1;
    std::string     tip_hash;
    size_t          rollback_window = kDefaultRollbackWin;
    int64_t         max_reorg_depth = kDefaultMaxReorgDepth;
    bool            track_mempool   = false;
    // #5: fetch raw blocks (getblock verbosity 0) and deserialize natively into
    // CBlock instead of pulling verbosity=2 JSON — much faster on a mainnet
    // genesis walk. Default on: the node must serve rpcserialversion=2 (raw with
    // witness + MWEB, the OYO/Litecoin default). Set false → the JSON path.
    bool            native_block_parse = true;
    std::deque<Block> blocks;                  // ring, last N applied blocks
    std::unordered_map<std::string, std::unique_ptr<Address>> addresses;  // by script_hex
    std::unordered_map<std::string, PendingTx> mempool;  // by txid
    // tx ids the mempool sync has already inspected and decided are not ours;
    // skipped on subsequent diffs to avoid re-fetching getrawtransaction.
    std::unordered_map<std::string, int64_t> mempool_seen_not_ours;
    // Transient-watcher mempool view (see MempoolTxView). Built for ALL mempool
    // txs during mempool_sync; scanned per request by oyo_chain_address_status so
    // an arbitrary address gets pending_in/out with zero node queries. Empty when
    // track_mempool is off.
    std::unordered_map<std::string, MempoolTxView> mempool_view;   // by txid
    // Three-state mirror lifecycle. meta(kMetaState) = bootstrap/indexing/working.
    // Bootstrap defers serving indexes and commits in batches; indexing builds
    // indexes; working commits per block. mirror_delta is accumulated inside an
    // open batch and folded into integer meta counters exactly once before COMMIT.
    bool    bootstrap_done      = false;
    bool    mirror_txn_open     = false;
    int64_t mirror_batch_count  = 0;
    int64_t mirror_batch_target = 1;
    MirrorDelta mirror_delta;
    std::vector<std::unique_ptr<OyoWalletImpl>> wallets;
    std::string     status_cache;
    std::string     address_status_cache;      // borrow buffer for oyo_chain_address_status
    SyncPerfStats   sync_perf;

    // MWEB extension. Keychains and addresses are chain-level; both are
    // dedup'd so that two wallets opened on the same seed share state
    // (one keychain, shared MwebAddress objects with multi-watcher).
    // mweb_addresses is keyed by bech32 stealth address (canonical-style
    // identity, real string), mweb_utxo_index by raw 33-byte commitment.
    std::vector<std::unique_ptr<MwebKeychain>>          mweb_keychains;
    std::unordered_map<std::string, std::unique_ptr<MwebAddress>>
                                                        mweb_addresses;
    std::unordered_map<std::array<uint8_t, 33>, MwebUtxoEntry, ArrayHash33>
                                                        mweb_utxo_index;

    // Persistent LevelDB mirror. MwebMirror owns the DB + current WriteBatch;
    // RegularMirror and BlockTrail write through it so event/meta/block keys
    // commit atomically.
    std::unique_ptr<MwebMirror>                         mweb_mirror;
    std::unique_ptr<RegularMirror>                      regular_mirror;
    std::unique_ptr<BlockTrail>                         block_trail;

    // Per-block MWEB events parallel to Block.events. Indexed by Block.height
    // through chain.blocks; we don't extend Block to keep the existing layout
    // untouched. Drift-free: pop_front on chain.blocks is mirrored here.
    std::deque<std::vector<MwebBlockEvent>>             mweb_events_per_block;

    // MWEB mempool: pending outputs (peg-in + pure MWEB) that aren't yet in
    // a block. Keyed by raw 33-byte commitment; only OUR matches end up here.
    // mempool_seen_not_ours dedupes failed RewindOutput attempts across diffs.
    std::unordered_map<std::array<uint8_t, 33>, MwebUtxoEntry, ArrayHash33>
                                                        mweb_mempool_pending;
    std::unordered_set<std::array<uint8_t, 33>, ArrayHash33>
                                                        mweb_mempool_seen_not_ours;
    // Pending OUT: confirmed UTXOs currently being spent by a mempool tx.
    // Keyed by raw 33-byte commitment; populated from the mempool tx walk.
    std::unordered_set<std::array<uint8_t, 33>, ArrayHash33>
                                                        mweb_mempool_pending_out;

    // Per-txid MWEB mempool contributions, decoded natively from the same
    // getrawtransaction(verbose).hex the canonical mempool sync already fetches
    // (the hex carries the MWEB extension under rpcserialversion=2). The two
    // pending sets above are rebuilt from this each poll — this is what lets the
    // engine drop the node-side oyo-mweb-mempool RPC. Evicted when a tx leaves
    // the node mempool (see MempoolHandleListResponse).
    struct MwebMempoolTx {
        // commitment (33B) + serialized mw::Output blob, one per MWEB output.
        std::vector<std::pair<std::array<uint8_t, 33>, std::vector<uint8_t>>> outputs;
        // input commitments (33B), one per MWEB input.
        std::vector<std::array<uint8_t, 33>> spends;
    };
    std::unordered_map<std::string, MwebMempoolTx>      mweb_mempool_tx_cache;
};

struct OyoCtxImpl {
    uint32_t    magic = kMagicCtx;
    std::mutex  mu;
    int32_t     last_err_code = 0;
    std::string last_err_msg;
    // Filesystem path under which per-chain mirrors live. Empty/unset keeps
    // dev/test state volatile.
    std::string workdir;
    std::vector<std::unique_ptr<OyoChainImpl>> chains;
    std::unordered_map<OYO_OP, std::unique_ptr<OyoOpImpl>> ops;
};

enum class OpKind : int32_t {
    Sync = 1, Rescan = 2, MempoolSync = 3,
    MwebBootstrap = 4, MwebSend = 6,
    RegularSend = 7, ExtPegIn = 8,
};
enum class SyncPhase : int32_t {
    Init = 0,
    WaitBlockchainInfo = 1,
    WaitAnchorHash = 2,
    WaitNextHash = 3,
    WaitBlock = 4,
    Done = 100,
};
enum class RescanPhase : int32_t {
    Init = 0,
    Done = 100,
};

enum class MempoolPhase : int32_t {
    Init = 0,
    WaitMempoolList = 1,
    WaitTxData = 2,
    Done = 100,
};

enum class MwebBootstrapPhase : int32_t {
    Init = 0,
    Done = 100,
};

enum class MwebSendPhase : int32_t {
    Init = 0,
    WaitFeeRate   = 1,   // estimatesmartfee in flight (skipped if rate override)
    WaitBroadcast = 2,
    Done = 100,
};

enum class RegularSendPhase : int32_t {
    Init = 0,
    WaitFeeRate   = 1,
    WaitBroadcast = 2,
    Done = 100,
};

enum class ExtPegInPhase : int32_t {
    Init = 0,
    WaitFeeRate   = 1,
    WaitBroadcast = 2,
    Done = 100,
};

struct OyoOpImpl {
    uint32_t        magic = kMagicOp;
    OyoCtxImpl*     ctx = nullptr;
    OyoChainImpl*   chain = nullptr;
    OyoWalletImpl*  wallet = nullptr;
    OpKind          kind;
    int32_t         state = OYO_OP_NEED_RPC;
    int32_t         err_code = 0;
    std::string     err_msg;
    std::string     rpc_req;
    std::string     result_json;
    int32_t         phase = 0;
    int64_t         target_height = -1;
    int64_t         rollback_start_height = -1;
    std::string     target_hash;
    int64_t         walk_height = -1;
    std::string     walk_hash;
    int             blocks_applied = 0;
    std::vector<std::string> rescan_scripts;  // if empty on Rescan: all wallet addresses

    // Mempool sync state.
    std::vector<std::string> mempool_added;       // txids to fetch
    size_t                   mempool_added_idx = 0;
    int64_t                  mempool_rolled_back = 0;
    int64_t                  mempool_added_count = 0;
    int64_t                  mempool_skipped     = 0;


    // Multi-recipient destination spec — populated when cfg.outputs[]
    // is present. For multi-output sends `send_to` / `send_amount` are
    // synthesized from the first entry (so legacy single-recipient
    // logging/result fields stay populated). All entries must be the
    // same address class (canonical XOR stealth) — mixed kinds are
    // rejected at dispatch time. send_all is incompatible with
    // outputs[] and rejected at parse time.
    struct OutputSpec {
        std::string address;
        int64_t     amount_sat = 0;
        bool        drain = false;   // P2.4: this output absorbs the
                                     // remainder (all inputs minus the
                                     // fixed outputs and fee). At most one
                                     // per send; canonical path only.
    };
    std::vector<OutputSpec> send_outputs;

    // Manual input selection — populated when cfg.inputs[] is present.
    // Empty => finalizers run their default largest-first auto-select
    // across all confirmed wallet UTXOs. Non-empty => ONLY these
    // outpoints / commitments are spent. Mixed canonical+MWEB inputs
    // in one tx aren't supported (mixed-kind is a separate roadmap
    // item); each path validates its own input shape at parse time.
    struct InputSpec {
        std::string txid;        // canonical: outpoint txid hex
        uint32_t    vout = 0;    // canonical: outpoint vout
        std::string commitment;  // mweb: stealth utxo commitment hex
    };
    std::vector<InputSpec> send_inputs;

    // Custom change address (P2.3) — when non-empty, canonical change is
    // sent to this caller-supplied address instead of a fresh wallet-owned
    // binding. Regular (canonical) send path only; MWEB / peg-in reject it.
    // Validated as a canonical destination at parse time.
    std::string change_address;

    // MWEB send state.
    std::string mweb_send_to;        // recipient bech32 (or first of outputs[])
    int64_t     mweb_send_amount = 0;
    int64_t     mweb_send_fee    = 0;
    int64_t     mweb_send_change = 0;
    int64_t     mweb_send_inputs_total = 0;
    std::vector<std::array<uint8_t, 33>> mweb_send_inputs_commit;  // commitments selected (bytes)
    std::string mweb_send_tx_hex;    // serialized CTransaction with mweb extension

    // Regular external send state (P2WPKH inputs → bech32 / legacy outputs).
    std::string reg_send_to;
    int64_t     reg_send_amount = 0;
    int64_t     reg_send_fee    = 0;
    int64_t     reg_send_change = 0;
    int64_t     reg_send_inputs_total = 0;
    std::vector<std::string> reg_send_inputs_outpoints;  // "txid:vout" of selected
    std::string reg_send_tx_hex;

    // External peg-in (R→M) state.
    std::string ext_pegin_to;          // recipient bech32 stealth
    int64_t     ext_pegin_amount = 0;  // recipient amount on MWEB side
    int64_t     ext_pegin_fee_budget = 0;  // legacy / explicit fee_sat override (0 = auto)
    int64_t     ext_pegin_mweb_fee = 0;
    int64_t     ext_pegin_canonical_fee = 0;
    int64_t     ext_pegin_change = 0;
    bool        ext_pegin_change_on_mweb = false;  // false → change_sat is canonical (R wallet)
    int64_t     ext_pegin_inputs_total = 0;
    std::vector<std::string> ext_pegin_inputs_outpoints;  // "txid:vout" selected
    std::string ext_pegin_tx_hex;

    // Fee-policy state (shared across send op kinds).
    //   fee_rate_sat_per_vb : resolved rate. 0 => not yet known; will be
    //                        looked up via estimatesmartfee + fallback 1.
    //                        Caller-provided override skips the lookup.
    //   dry_run             : if true, op finishes after build+sign with
    //                        a result-only payload (no broadcast). Powers
    //                        the /api/wallet/estimate-send endpoint.
    //   send_all            : if true, recipient_amount is treated as
    //                        "send everything we have minus fee" — picks
    //                        all confirmed UTXOs, no on-side change.
    uint64_t fee_rate_sat_per_vb = 0;
    bool     dry_run             = false;
    bool     send_all            = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string ToHexLower(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out; out.resize(len * 2);
    for (size_t i = 0; i < len; i++) {
        out[2*i]   = hex[data[i] >> 4];
        out[2*i+1] = hex[data[i] & 0x0f];
    }
    return out;
}

std::string LowerHex(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
    return s;
}

int64_t LtcStringToSat(const std::string& s) {
    if (s.empty()) return 0;
    bool neg = false; size_t i = 0;
    if (s[0] == '-') { neg = true; i++; } else if (s[0] == '+') i++;
    int64_t whole = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { whole = whole*10 + (s[i]-'0'); i++; }
    int64_t frac = 0, frac_div = 1;
    if (i < s.size() && s[i] == '.') {
        i++; int d = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && d < 8) {
            frac = frac*10 + (s[i]-'0'); frac_div *= 10; d++; i++;
        }
        while (d < 8) { frac *= 10; frac_div *= 10; d++; }
    }
    int64_t sat = whole * kCoin + (frac * kCoin) / frac_div;
    return neg ? -sat : sat;
}

UniValue ParseJson(const uint8_t* data, size_t len) {
    UniValue v;
    if (!v.read(std::string(reinterpret_cast<const char*>(data), len))) {
        throw std::runtime_error("invalid json");
    }
    return v;
}

std::string LdbPutU64(std::string s, uint64_t v) {
    for (int i = 7; i >= 0; --i) s.push_back(char((v >> (i * 8)) & 0xff));
    return s;
}
uint64_t LdbGetU64(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v = (v << 8) | uint8_t(s[off + i]);
    return v;
}
void LdbAppendU32(std::string& s, uint32_t v) {
    s.push_back(char((v >> 24) & 0xff));
    s.push_back(char((v >> 16) & 0xff));
    s.push_back(char((v >> 8) & 0xff));
    s.push_back(char(v & 0xff));
}
bool LdbStartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}
std::string LdbBytes(const std::vector<uint8_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}
uint32_t LdbGetU32(const std::string& s, size_t off) {
    return (uint32_t(uint8_t(s[off]))   << 24) | (uint32_t(uint8_t(s[off+1])) << 16) |
           (uint32_t(uint8_t(s[off+2])) <<  8) |  uint32_t(uint8_t(s[off+3]));
}
// hash20(x) = first 20 bytes of SHA-256(x). Used as a fixed-width index key for
// scripts/outpoints/commitments: the full bytes are never read back from these
// indexes (callers always supply them and we only MATCH), so we store a 160-bit
// digest. 160 bits is the same collision resistance the chain itself relies on
// for Hash160 addresses; this is a node-rebuildable local cache with no
// exact-compare fallback by design (a collision could only mis-display a local
// balance, never cause on-chain loss — the node validates real spends). MWEB
// commitments are additionally Pedersen-bound to value+blinding, so a colliding
// valid on-chain commitment cannot be constructed at all.
std::string Hash20(const std::vector<uint8_t>& v) {
    unsigned char h[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(v.data(), v.size()).Finalize(h);
    return std::string(reinterpret_cast<const char*>(h), 20);
}
class MwebMirror {
public:
    // ASCII table prefixes (cf. Litecoin Core txdb 'C'/'b'/'f'...). Lowercase =
    // regular, uppercase = MWEB. Journals (o/s/S) are reorg-only and pruned;
    // indexes (a/x/X) and the MWEB output journal (O) are full. No prefix is the
    // max byte, so Tip() is range-based, not SeekToLast().
    static constexpr unsigned char kMeta                  = 'm';
    static constexpr unsigned char kOutput                = 'o'; // reg output journal:  id -> hash20(script)
    static constexpr unsigned char kScript                = 'a'; // reg address index:   hash20(script)|id -> outpoint|amount
    static constexpr unsigned char kSpend                 = 's'; // reg spend journal:   id -> hash20(outpoint)
    static constexpr unsigned char kSpentByOutpoint       = 'x'; // reg spent index:     hash20(outpoint) -> spend_id
    static constexpr unsigned char kMwebOutput            = 'O'; // mweb output journal: id -> commitment|output_id|receiver|message
    static constexpr unsigned char kMwebSpend             = 'S'; // mweb spend journal:  id -> hash20(commitment)
    static constexpr unsigned char kMwebSpentByCommitment = 'X'; // mweb spent index:    hash20(commitment) -> spend_id
    static constexpr unsigned char kBlock                 = 'b'; // block trail:         u32(height) -> block_hash

    explicit MwebMirror(leveldb::DB* db,
                        std::unique_ptr<leveldb::Cache> cache,
                        std::unique_ptr<const leveldb::FilterPolicy> filter)
        : db_(db), cache_(std::move(cache)), filter_(std::move(filter)) {}
    ~MwebMirror() {
        batch_.reset();
        delete db_;
        db_ = nullptr;
        // Volatile dev/test mode (empty/":memory:" workdir) synthesizes a temp
        // LevelDB dir; drop it on close so repeated opens (e.g. the e2e suite)
        // don't leak directories under /tmp. Production paths are never temp.
        if (!owned_temp_path_.empty()) {
            leveldb::DestroyDB(owned_temp_path_, leveldb::Options());
        }
    }

    static std::unique_ptr<MwebMirror> Open(const std::string& path) {
        static std::atomic<int> mem_counter{0};
        std::string p = path;
        bool is_temp = false;
        if (p == ":memory:" || p.empty()) {
            p = std::string("/tmp/oyoltc-leveldb-") + std::to_string(getpid()) +
                "-" + std::to_string(mem_counter.fetch_add(1) + 1);
            is_temp = true;
            // Clear any stale dir left by a previously crashed volatile run so a
            // corrupt remnant can't wedge the fresh open.
            leveldb::DestroyDB(p, leveldb::Options());
        }
        leveldb::Options opt;
        opt.create_if_missing = true;
        opt.error_if_exists = false;
        // Fail fast on detected corruption (manifest/compaction) instead of
        // limping along — the mirror is node-rebuildable, so a hard error that
        // forces a clean rebuild is safer than silently serving bad data over a
        // long-running deployment.
        opt.paranoid_checks = true;
        opt.write_buffer_size = 64 * 1024 * 1024;
        opt.max_file_size = 64 * 1024 * 1024;
        opt.max_open_files = 4000;
        opt.block_size = 16 * 1024;
        opt.block_restart_interval = 16;
        auto cache = std::unique_ptr<leveldb::Cache>(leveldb::NewLRUCache(128 * 1024 * 1024));
        auto filter = std::unique_ptr<const leveldb::FilterPolicy>(leveldb::NewBloomFilterPolicy(10));
        opt.block_cache = cache.get();
        opt.filter_policy = filter.get();
        leveldb::DB* db = nullptr;
        leveldb::Status st = leveldb::DB::Open(opt, p, &db);
        if (!st.ok()) throw std::runtime_error("LevelDB mirror open: " + st.ToString());
        auto m = std::unique_ptr<MwebMirror>(new MwebMirror(db, std::move(cache), std::move(filter)));
        if (is_temp) m->owned_temp_path_ = p;
        m->InitMeta();
        return m;
    }

    void Begin() {
        if (!batch_) batch_.reset(new leveldb::WriteBatch());
    }
    // sync=false is the steady-state path: the mirror is a node-derived cache,
    // the LevelDB log keeps each committed batch atomic across a crash, and the
    // block trail lets a restart resume from the last committed height — so an
    // unclean exit costs only a few re-walked blocks, never a torn block. sync=true
    // is used once on clean shutdown to fsync the final batch to disk.
    void Commit(bool sync = false) {
        if (!batch_) return;
        leveldb::WriteOptions wo;
        wo.sync = sync;
        leveldb::Status st = db_->Write(wo, batch_.get());
        batch_.reset();
        if (!st.ok()) throw std::runtime_error("LevelDB mirror commit: " + st.ToString());
    }
    void Rollback() { batch_.reset(); }

    void Put(const std::string& key, const std::string& value) {
        if (batch_) batch_->Put(key, value);
        else {
            leveldb::WriteOptions wo;
            wo.sync = false;
            leveldb::Status st = db_->Put(wo, key, value);
            if (!st.ok()) throw std::runtime_error("LevelDB put: " + st.ToString());
        }
    }
    void Delete(const std::string& key) {
        if (batch_) batch_->Delete(key);
        else {
            leveldb::WriteOptions wo;
            wo.sync = false;
            leveldb::Status st = db_->Delete(wo, key);
            if (!st.ok()) throw std::runtime_error("LevelDB delete: " + st.ToString());
        }
    }
    bool Get(const std::string& key, std::string* value) const {
        leveldb::ReadOptions ro;
        leveldb::Status st = db_->Get(ro, key, value);
        if (st.IsNotFound()) return false;
        if (!st.ok()) throw std::runtime_error("LevelDB get: " + st.ToString());
        return true;
    }
    std::unique_ptr<leveldb::Iterator> NewIterator() const {
        leveldb::ReadOptions ro;
        return std::unique_ptr<leveldb::Iterator>(db_->NewIterator(ro));
    }

    static std::string MetaKey(int64_t key) {
        return LdbPutU64(std::string(1, char(kMeta)), uint64_t(key));
    }
    static std::string OutputKey(int64_t id) {
        return LdbPutU64(std::string(1, char(kOutput)), uint64_t(id));
    }
    static std::string SpendKey(int64_t id) {
        return LdbPutU64(std::string(1, char(kSpend)), uint64_t(id));
    }
    static std::string ScriptPrefix(const std::vector<uint8_t>& script) {
        return std::string(1, char(kScript)) + Hash20(script);   // 'a' | hash20(script) (21 bytes)
    }
    static std::string ScriptKey(const std::vector<uint8_t>& script, int64_t id) {
        return LdbPutU64(ScriptPrefix(script), uint64_t(id));
    }
    // Reorg deletes the address-index key from the hash stored in the output
    // journal value (no re-hash). hash20 is the exact 20-byte digest string.
    static std::string ScriptKeyFromHash(const std::string& hash20, int64_t id) {
        return LdbPutU64(std::string(1, char(kScript)) + hash20, uint64_t(id));
    }
    static std::string OutpointSpentKey(const std::vector<uint8_t>& outpoint) {
        return std::string(1, char(kSpentByOutpoint)) + Hash20(outpoint);
    }
    static std::string OutpointSpentKeyFromHash(const std::string& hash20) {
        return std::string(1, char(kSpentByOutpoint)) + hash20;
    }
    static std::string MwebOutputKey(int64_t id) {
        return LdbPutU64(std::string(1, char(kMwebOutput)), uint64_t(id));
    }
    static std::string MwebSpendKey(int64_t id) {
        return LdbPutU64(std::string(1, char(kMwebSpend)), uint64_t(id));
    }
    static std::string MwebCommitSpentKey(const std::vector<uint8_t>& commit) {
        return std::string(1, char(kMwebSpentByCommitment)) + Hash20(commit);
    }
    static std::string MwebCommitSpentKeyFromHash(const std::string& hash20) {
        return std::string(1, char(kMwebSpentByCommitment)) + hash20;
    }
    static std::string BlockKey(int64_t height) {
        std::string k(1, char(kBlock));
        LdbAppendU32(k, uint32_t(height));   // height fits u32 (id scheme caps height < 2^32)
        return k;
    }

    void AppendOutput(int64_t id,
                      const std::vector<uint8_t>& commit_bytes,
                      const std::vector<uint8_t>& output_id,
                      const std::vector<uint8_t>& receiver_pubkey,
                      const std::vector<uint8_t>& message) {
        std::string v = LdbBytes(commit_bytes);
        v.append(LdbBytes(output_id));
        v.append(LdbBytes(receiver_pubkey));
        v.append(LdbBytes(message));
        Put(MwebOutputKey(id), v);
    }
    void AppendSpend(int64_t id, const std::vector<uint8_t>& commit_bytes) {
        std::string sid = LdbPutU64(std::string(), uint64_t(id));
        Put(MwebSpendKey(id), Hash20(commit_bytes));          // journal: id -> hash20(commitment)
        Put(MwebCommitSpentKey(commit_bytes), sid);           // index:   X|hash20(commitment) -> spend_id
    }
    MirrorDelta DeleteAbove(int64_t height) {
        const int64_t id_floor = (height + 1) << 32;
        MirrorDelta d;
        DeleteRangeById(kMwebOutput, id_floor, [&](const std::string&, const std::string&) {
            d.mweb_outputs++;
        });
        DeleteRangeById(kMwebSpend, id_floor, [&](const std::string&, const std::string& v) {
            if (v.size() == 20) Delete(MwebCommitSpentKeyFromHash(v));  // v = hash20(commitment)
            d.mweb_spends++;
        });
        return d;
    }

    using AllCallback = std::function<void(
        const std::vector<uint8_t>&, const std::vector<uint8_t>&,
        const std::vector<uint8_t>&, const std::vector<uint8_t>&, int64_t, int64_t)>;
    void ForEachAll(const AllCallback& cb, int64_t from_height = 0) {
        auto it = NewIterator();
        const std::string prefix(1, char(kMwebOutput));
        // ids are height-major (id = height<<32 | ordinal), so seek straight to
        // the first output at/after from_height — a wallet birth height skips
        // the entire pre-birth prefix of the journal.
        const std::string start = from_height > 0
            ? LdbPutU64(prefix, uint64_t(from_height) << 32)
            : prefix;
        for (it->Seek(start); it->Valid() && LdbStartsWith(it->key().ToString(), prefix); it->Next()) {
            std::string k = it->key().ToString();
            std::string v = it->value().ToString();
            if (k.size() != 9 || v.size() < 98) continue;
            int64_t id = int64_t(LdbGetU64(k, 1));
            std::vector<uint8_t> commit(reinterpret_cast<const uint8_t*>(v.data()),
                                        reinterpret_cast<const uint8_t*>(v.data()) + 33);
            std::vector<uint8_t> output_id(reinterpret_cast<const uint8_t*>(v.data()) + 33,
                                           reinterpret_cast<const uint8_t*>(v.data()) + 65);
            std::vector<uint8_t> receiver(reinterpret_cast<const uint8_t*>(v.data()) + 65,
                                          reinterpret_cast<const uint8_t*>(v.data()) + 98);
            std::vector<uint8_t> message(reinterpret_cast<const uint8_t*>(v.data()) + 98,
                                         reinterpret_cast<const uint8_t*>(v.data()) + v.size());
            int64_t spent_height = 0;
            std::string spent_id;
            if (Get(MwebCommitSpentKey(commit), &spent_id) && spent_id.size() == 8)
                spent_height = int64_t(LdbGetU64(spent_id, 0)) >> 32;
            cb(commit, output_id, receiver, message, id >> 32, spent_height);
        }
        if (!it->status().ok()) throw std::runtime_error("LevelDB MWEB scan: " + it->status().ToString());
    }

    void Counts(int64_t& total, int64_t& unspent) {
        int64_t out = MetaGet(kMetaMwebOutputs, 0);
        int64_t spend = MetaGet(kMetaMwebSpends, 0);
        total = out;
        unspent = out - spend;
    }
    int64_t MetaGet(int64_t key, int64_t def = 0) const {
        std::string v;
        if (!Get(MetaKey(key), &v)) return def;
        return v.size() == 8 ? int64_t(LdbGetU64(v, 0)) : def;
    }
    void MetaSet(int64_t key, int64_t value) {
        Put(MetaKey(key), LdbPutU64(std::string(), uint64_t(value)));
    }
    void MetaAdd(int64_t key, int64_t delta) {
        if (delta == 0) return;
        MetaSet(key, MetaGet(key, 0) + delta);
    }
    void ApplyDelta(const MirrorDelta& d) {
        MetaAdd(kMetaRegularOutputs, d.regular_outputs);
        MetaAdd(kMetaRegularSpends,  d.regular_spends);
        MetaAdd(kMetaMwebOutputs,    d.mweb_outputs);
        MetaAdd(kMetaMwebSpends,     d.mweb_spends);
    }

private:
    template<typename Fn>
    void DeleteRangeById(unsigned char prefix_ch, int64_t id_floor, const Fn& fn) {
        const std::string prefix(1, char(prefix_ch));
        const std::string start = LdbPutU64(prefix, uint64_t(id_floor));
        auto it = NewIterator();
        for (it->Seek(start); it->Valid() && LdbStartsWith(it->key().ToString(), prefix); it->Next()) {
            std::string k = it->key().ToString();
            std::string v = it->value().ToString();
            fn(k, v);
            Delete(k);
        }
        if (!it->status().ok()) throw std::runtime_error("LevelDB tail delete: " + it->status().ToString());
    }
    void InitMeta() {
        // Schema guard. A compatible mirror carries kMirrorSchemaVersion; an
        // incompatible/older one (different key layout) MUST be wiped, never
        // silently reused or mistaken for empty.
        std::string ver;
        if (Get(MetaKey(kMetaSchemaVersion), &ver)) {
            int64_t v = ver.size() == 8 ? int64_t(LdbGetU64(ver, 0)) : -1;
            if (v != kMirrorSchemaVersion)
                throw std::runtime_error(
                    "LevelDB mirror schema version " + std::to_string(v) +
                    " != expected " + std::to_string(kMirrorSchemaVersion) + "; wipe required");
            return;  // compatible existing mirror; counters already present
        }
        // No version key: truly fresh (empty) or an old prefix-byte format.
        {
            auto it = NewIterator();
            it->SeekToFirst();
            if (it->Valid())
                throw std::runtime_error(
                    "LevelDB mirror has data but no schema version (incompatible format); wipe required");
        }
        // Fresh DB: stamp version + initialise meta integers.
        MetaSet(kMetaSchemaVersion, kMirrorSchemaVersion);
        for (int64_t k : {int64_t(kMetaState), int64_t(kMetaRegularOutputs),
                          int64_t(kMetaRegularSpends), int64_t(kMetaMwebOutputs),
                          int64_t(kMetaMwebSpends), int64_t(kMetaRegularPruneFloor)}) {
            MetaSet(k, 0);
        }
    }
    leveldb::DB* db_ = nullptr;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<leveldb::WriteBatch> batch_;
    std::string owned_temp_path_;   // non-empty only for volatile dev/test dirs
};

class RegularMirror {
public:
    static constexpr int64_t kFlagPegOut   = 1;
    static constexpr int64_t kFlagCoinbase = 2;

    explicit RegularMirror(MwebMirror* owner) : owner_(owner) {}
    // write_journal: write the reorg-only output journal ('o'). During the
    // bootstrap walk, blocks already buried deeper than max_reorg_depth below the
    // node tip are final (the node will never reorg them), so their journal
    // entries would only ever be written-then-pruned — we skip writing them
    // entirely. The address index ('a', full history) is ALWAYS written.
    void AppendOutput(int64_t id, const std::vector<uint8_t>& outpoint,
                      const std::vector<uint8_t>& script, int64_t amount, bool write_journal) {
        if (write_journal)
            owner_->Put(MwebMirror::OutputKey(id), Hash20(script)); // journal: id -> hash20(script)
        std::string idx_val = LdbBytes(outpoint);
        idx_val = LdbPutU64(idx_val, uint64_t(amount));
        owner_->Put(MwebMirror::ScriptKey(script, id), idx_val);    // a|hash20(script)|id -> outpoint|amount
    }
    void AppendSpend(int64_t id, const std::vector<uint8_t>& outpoint, bool write_journal) {
        std::string sid = LdbPutU64(std::string(), uint64_t(id));
        if (write_journal)
            owner_->Put(MwebMirror::SpendKey(id), Hash20(outpoint)); // journal: id -> hash20(outpoint)
        owner_->Put(MwebMirror::OutpointSpentKey(outpoint), sid);    // index:   x|hash20(outpoint) -> spend_id
    }
    MirrorDelta DeleteAbove(int64_t height) {
        const int64_t id_floor = (height + 1) << 32;
        MirrorDelta d;
        DeleteOutputRange(id_floor, d);
        DeleteSpendRange(id_floor, d);
        return d;
    }
    // Drop output/spend JOURNAL rows (o/s) with height < keep_floor_height. These
    // are reorg-scratch only (no Get reader); the address index (a), spent index
    // (x), block trail and meta counters are NOT touched (counters are meta
    // integers, not row counts). Bounded by max_deletes so the WriteBatch stays
    // small. Returns true if fully caught up below keep_floor (so the caller can
    // advance the persisted prune floor only when the region is truly clean).
    bool PruneJournalsBelow(int64_t start_floor_height, int64_t keep_floor_height, size_t max_deletes) {
        const int64_t start_id = start_floor_height << 32;
        const int64_t floor_id = keep_floor_height << 32;
        size_t budget = max_deletes;
        PrunePrefixBelow(MwebMirror::kOutput, start_id, floor_id, budget);
        PrunePrefixBelow(MwebMirror::kSpend,  start_id, floor_id, budget);
        return budget > 0;
    }
    using ByScriptCallback = std::function<void(
        const std::vector<uint8_t>&, int64_t, int64_t, int64_t)>;
    void ForEachUnspentByScript(const std::vector<uint8_t>& script,
                                const ByScriptCallback& cb) {
        ScanScript(script, [&](const std::vector<uint8_t>& outpoint, int64_t amount, int64_t id, int64_t flags) {
            std::string spent_id;
            if (owner_->Get(MwebMirror::OutpointSpentKey(outpoint), &spent_id)) return;
            cb(outpoint, amount, id >> 32, flags);
        });
    }
    void Counts(int64_t& total, int64_t& unspent) {
        int64_t out = owner_->MetaGet(kMetaRegularOutputs, 0);
        int64_t spend = owner_->MetaGet(kMetaRegularSpends, 0);
        total = out;
        unspent = out - spend;
    }
    using HistoryCallback = std::function<void(
        const std::vector<uint8_t>&, int64_t, int64_t, int64_t, int64_t)>;
    // Visits each output of the script (newest-first) with its spent height
    // (0 = unspent). `limit` bounds it to the newest `limit` outputs (0 =
    // all); returns true when more outputs exist beyond the limit.
    bool ForEachByScript(const std::vector<uint8_t>& script,
                         const HistoryCallback& cb, size_t limit = 0) {
        return ScanScript(script, [&](const std::vector<uint8_t>& outpoint, int64_t amount, int64_t id, int64_t flags) {
            int64_t spent_height = 0;
            std::string spent_id;
            if (owner_->Get(MwebMirror::OutpointSpentKey(outpoint), &spent_id) && spent_id.size() == 8)
                spent_height = int64_t(LdbGetU64(spent_id, 0)) >> 32;
            cb(outpoint, amount, id >> 32, spent_height, flags);
        }, limit);
    }
private:
    // Walk an address's outputs NEWEST-first (descending id, i.e. descending
    // height). Keys are 'a'|hash20|id sorted ascending, so we seek one past
    // this script's range and step backwards with Prev().
    //
    // `limit` bounds the walk to the newest `limit` outputs (0 = unbounded).
    // The explorer passes a small limit: a heavily-reused address can have
    // millions of outputs, and an unbounded walk + per-output spent-Get took
    // >20 min while holding the engine lock. Older outputs are likelier already
    // spent, so the newest window captures ~all the live balance; for an
    // address with more, the bounded view is flagged partial. Wallet rescan
    // passes 0 (it needs the exact, complete UTXO set). Returns true when the
    // walk stopped at `limit` with more outputs remaining (partial view).
    template<typename Fn>
    bool ScanScript(const std::vector<uint8_t>& script, const Fn& fn, size_t limit = 0) {
        const std::string prefix = MwebMirror::ScriptPrefix(script);
        // Sentinel one past this script's last key. id is never all-0xFF (its
        // high 32 bits are the block height), so 'a'|hash20|FF..FF sorts above
        // every real row for the script but below the next prefix; Seek lands
        // beyond the range and Prev() drops onto the highest-id (newest) row.
        std::string past = prefix; past.append(8, '\xff');
        auto it = owner_->NewIterator();
        it->Seek(past);
        if (it->Valid()) it->Prev(); else it->SeekToLast();
        size_t seen = 0;
        for (; it->Valid(); it->Prev()) {
            std::string k = it->key().ToString();
            if (!LdbStartsWith(k, prefix)) break;
            std::string v = it->value().ToString();
            if (k.size() != prefix.size() + 8 || v.size() < 44) continue;
            int64_t id = int64_t(LdbGetU64(k, prefix.size()));
            std::vector<uint8_t> outpoint(reinterpret_cast<const uint8_t*>(v.data()),
                                          reinterpret_cast<const uint8_t*>(v.data()) + 36);
            int64_t amount = int64_t(LdbGetU64(v, 36));
            fn(outpoint, amount, id, id & 0xff);
            if (limit && ++seen >= limit) {
                // Cap hit. Step once more to report whether the view is partial.
                it->Prev();
                bool more = it->Valid() && LdbStartsWith(it->key().ToString(), prefix);
                if (!it->status().ok())
                    throw std::runtime_error("LevelDB script scan: " + it->status().ToString());
                return more;
            }
        }
        if (!it->status().ok()) throw std::runtime_error("LevelDB script scan: " + it->status().ToString());
        return false;
    }
    void DeleteOutputRange(int64_t id_floor, MirrorDelta& d) {
        const std::string prefix(1, char(MwebMirror::kOutput));
        auto it = owner_->NewIterator();
        for (it->Seek(LdbPutU64(prefix, uint64_t(id_floor)));
             it->Valid() && LdbStartsWith(it->key().ToString(), prefix); it->Next()) {
            std::string k = it->key().ToString();
            std::string v = it->value().ToString();   // v = hash20(script)
            if (k.size() == 9 && v.size() == 20) {
                int64_t id = int64_t(LdbGetU64(k, 1));
                owner_->Delete(MwebMirror::ScriptKeyFromHash(v, id));
            }
            owner_->Delete(k);
            d.regular_outputs++;
        }
        if (!it->status().ok())
            throw std::runtime_error("LevelDB output tail delete: " + it->status().ToString());
    }
    void DeleteSpendRange(int64_t id_floor, MirrorDelta& d) {
        const std::string prefix(1, char(MwebMirror::kSpend));
        auto it = owner_->NewIterator();
        for (it->Seek(LdbPutU64(prefix, uint64_t(id_floor)));
             it->Valid() && LdbStartsWith(it->key().ToString(), prefix); it->Next()) {
            std::string k = it->key().ToString();
            std::string v = it->value().ToString();   // v = hash20(outpoint)
            if (v.size() == 20) owner_->Delete(MwebMirror::OutpointSpentKeyFromHash(v));
            owner_->Delete(k);
            d.regular_spends++;
        }
        if (!it->status().ok())
            throw std::runtime_error("LevelDB spend tail delete: " + it->status().ToString());
    }
    // Delete journal rows of one prefix with id in [start_id, floor_id), up to
    // budget. CRITICAL: Seek to start_id (the persisted prune watermark), NOT to
    // the prefix start — everything below the watermark is already deleted, so
    // seeking from the prefix would scan O(accumulated delete-tombstones) each
    // block (grows with chain height until compaction). Seeking to the watermark
    // jumps straight to the live tail. ids are ascending → stop at floor_id.
    void PrunePrefixBelow(unsigned char prefix_ch, int64_t start_id, int64_t floor_id, size_t& budget) {
        if (budget == 0) return;
        const std::string prefix(1, char(prefix_ch));
        auto it = owner_->NewIterator();
        for (it->Seek(LdbPutU64(prefix, uint64_t(start_id))); it->Valid() && budget > 0; it->Next()) {
            const std::string k = it->key().ToString();
            if (!LdbStartsWith(k, prefix) || k.size() != 9) break;
            if (int64_t(LdbGetU64(k, 1)) >= floor_id) break;
            owner_->Delete(k);
            --budget;
        }
        if (!it->status().ok())
            throw std::runtime_error("LevelDB journal prune: " + it->status().ToString());
    }
    MwebMirror* owner_ = nullptr;
};

class BlockTrail {
public:
    explicit BlockTrail(MwebMirror* owner) : owner_(owner) {}
    void Put(int64_t height, const std::vector<uint8_t>& hash) {
        owner_->Put(MwebMirror::BlockKey(height), LdbBytes(hash));
    }
    std::vector<uint8_t> HashAt(int64_t height) {
        std::string v;
        if (!owner_->Get(MwebMirror::BlockKey(height), &v)) return {};
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(v.data()),
                                    reinterpret_cast<const uint8_t*>(v.data()) + v.size());
    }
    int64_t DeleteAbove(int64_t height) {
        int64_t n = 0;
        const std::string prefix(1, char(MwebMirror::kBlock));
        auto it = owner_->NewIterator();
        for (it->Seek(MwebMirror::BlockKey(height + 1));
             it->Valid() && LdbStartsWith(it->key().ToString(), prefix); it->Next()) {
            owner_->Delete(it->key().ToString());
            n++;
        }
        if (!it->status().ok()) throw std::runtime_error("LevelDB block delete: " + it->status().ToString());
        return n;
    }
    int64_t Tip() {
        auto it = owner_->NewIterator();
        // 'b' is not the max prefix, so SeekToLast() would land on another table.
        // Seek to the first key past the block range ('b'+1 = 'c') and step back
        // to the highest block key.
        it->Seek(std::string(1, char(MwebMirror::kBlock + 1)));
        if (it->Valid()) it->Prev(); else it->SeekToLast();
        if (!it->Valid()) return -1;
        std::string k = it->key().ToString();
        if (!LdbStartsWith(k, std::string(1, char(MwebMirror::kBlock))) || k.size() != 5) return -1;
        return int64_t(LdbGetU32(k, 1));
    }
private:
    MwebMirror* owner_ = nullptr;
};

// Helper: ensure a directory exists (recursive mkdir, treating "already
// exists" as success). Used at chain_open to materialize ctx.workdir.
bool EnsureDirExists(const std::string& path) {
    if (path.empty()) return true;
    if (mkdir(path.c_str(), 0700) == 0) return true;
    if (errno == EEXIST) {
        struct stat st{};
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    return false;
}

std::string NormalizeNetwork(const std::string& n) {
    if (n == "main" || n == "mainnet") return "main";
    if (n == "test" || n == "testnet" || n == "testnet4") return "test";
    if (n == "regtest") return "regtest";
    return n;
}
std::string NetworkHrp(const std::string& n) {
    if (n == "main")    return "ltc";
    if (n == "regtest") return "rltc";
    return "tltc"; // testnet and other test nets
}

std::string EncodeP2WPKH(const std::string& hrp, const uint8_t* h160, size_t h160_len) {
    std::vector<uint8_t> conv;
    conv.reserve(1 + (h160_len * 8 + 4) / 5);
    conv.push_back(0); // witness version 0
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < h160_len; i++) {
        acc = (acc << 8) | h160[i]; bits += 8;
        while (bits >= 5) { bits -= 5; conv.push_back((acc >> bits) & 0x1f); }
    }
    if (bits > 0) conv.push_back((acc << (5 - bits)) & 0x1f);
    return bech32::Encode(bech32::Encoding::BECH32, hrp, conv);
}

std::vector<uint8_t> HexToBytes(const std::string& s) {
    std::vector<uint8_t> out;
    if (s.size() % 2 != 0) return out;
    out.reserve(s.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nyb(s[i]), b = nyb(s[i+1]);
        if (a < 0 || b < 0) { out.clear(); return out; }
        out.push_back(uint8_t((a << 4) | b));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Derivation (oyo_v1) — byte-compatible with existing Go prototype.
// ---------------------------------------------------------------------------

// SHA-256("oyo_mweb_v1_seed" || 0x00 || seed_string) — produces the
// 32-byte MWEB master used by DeriveScanSecret/DeriveSpendSecret. Lets
// MWEB and Universal wallets accept the same free-form seed string as
// Regular wallets (oyo_v1 canonical chain), instead of forcing the user
// to come up with 64 random hex chars. The domain tag separates this
// chain from the canonical oyo_v1 derivation so the same seed string
// can't accidentally collide between the two sides.
inline std::array<uint8_t, 32> MwebMasterFromSeedString(const std::vector<uint8_t>& seed_string) {
    CSHA256 h;
    const char* tag = "oyo_mweb_v1_seed";
    h.Write(reinterpret_cast<const unsigned char*>(tag), std::strlen(tag));
    unsigned char z = 0;
    h.Write(&z, 1);
    if (!seed_string.empty()) h.Write(seed_string.data(), seed_string.size());
    std::array<uint8_t, 32> out{};
    h.Finalize(out.data());
    return out;
}

// Derives the private key for a single (seed, network, index) slot of an
// oyo_v1 regular external wallet. Same SHA-256 chain that produced the
// pubkey at create-time; first valid scalar wins (16-nonce retry handles
// the rare case where SHA-256 lands outside the secp256k1 curve order).
bool DeriveOyoV1Key(const std::vector<uint8_t>& seed,
                    const std::string& network,
                    int index,
                    CKey& out_key) {
    for (int nonce = 0; nonce < 16; ++nonce) {
        CSHA256 h;
        const char* tag = "oyo external regular wallet v1";
        h.Write(reinterpret_cast<const unsigned char*>(tag), std::strlen(tag));
        unsigned char z = 0;
        h.Write(&z, 1);
        if (!seed.empty()) h.Write(seed.data(), seed.size());
        h.Write(&z, 1);
        h.Write(reinterpret_cast<const unsigned char*>(network.data()), network.size());
        h.Write(&z, 1);
        std::string idx = std::to_string(index);
        h.Write(reinterpret_cast<const unsigned char*>(idx.data()), idx.size());
        h.Write(&z, 1);
        std::string nn = std::to_string(nonce);
        h.Write(reinterpret_cast<const unsigned char*>(nn.data()), nn.size());
        unsigned char sum[32];
        h.Finalize(sum);
        out_key.Set(sum, sum + 32, /*compressed=*/true);
        if (!out_key.IsValid()) continue;
        CPubKey pk = out_key.GetPubKey();
        if (!pk.IsValid() || !pk.IsCompressed()) continue;
        return true;
    }
    return false;
}

bool DeriveOyoV1Pubkey(const std::vector<uint8_t>& seed,
                      const std::string& network,
                      int index,
                      std::vector<uint8_t>& out_pub) {
    CKey k;
    if (!DeriveOyoV1Key(seed, network, index, k)) return false;
    CPubKey pk = k.GetPubKey();
    out_pub.assign(pk.begin(), pk.end());
    return true;
}

// ---------------------------------------------------------------------------
// Handle validation
// ---------------------------------------------------------------------------

OyoCtxImpl*    AsCtx(OYO_CTX h)    { auto* p = reinterpret_cast<OyoCtxImpl*>(h);    return (p && p->magic == kMagicCtx)    ? p : nullptr; }
OyoChainImpl*  AsChain(OYO_CHAIN h){ auto* p = reinterpret_cast<OyoChainImpl*>(h);  return (p && p->magic == kMagicChain)  ? p : nullptr; }
OyoWalletImpl* AsWallet(OYO_WALLET h){ auto* p = reinterpret_cast<OyoWalletImpl*>(h); return (p && p->magic == kMagicWallet) ? p : nullptr; }
OyoOpImpl*     AsOp(OYO_OP h)      { auto* p = reinterpret_cast<OyoOpImpl*>(h);     return (p && p->magic == kMagicOp)     ? p : nullptr; }

int32_t Fail(OyoCtxImpl* ctx, int32_t code, const std::string& msg) {
    if (ctx) { ctx->last_err_code = code; ctx->last_err_msg = msg; }
    return code;
}

// ---------------------------------------------------------------------------
// Single-writer UTXO mutation helpers — the ONLY place addr.utxos changes.
// Every path that mutates state goes through these so watcher notifications
// and balance cache stay consistent.
// ---------------------------------------------------------------------------

void NotifyWatchersLocked(Address& addr, int64_t delta) {
    for (OyoWalletImpl* w : addr.watchers) {
        w->balance_sat += delta;
        w->revision++;
    }
}

void SetAddressDesyncLocked(Address& addr, bool desync) {
    if (addr.desync == desync) return;
    addr.desync = desync;
    NotifyWatchersLocked(addr, 0);   // revision bump, no balance change
}

// Bumps revision without touching balance. Used after pending-only changes.
void BumpRevisionLocked(Address& addr) {
    NotifyWatchersLocked(addr, 0);
}

// AddUtxo: records a brand-new UTXO. confirmed=true means it came from a
// block; confirmed=false means it lives in mempool only.
// Idempotent: dup add (same key, same confirmed-state) is a no-op.
bool AddUtxoLocked(Address& addr,
                   const UtxoKey& key,
                   int64_t amount_sat,
                   int64_t height,
                   bool confirmed,
                   bool is_pegout_output = false,
                   bool is_coinbase = false) {
    if (addr.utxos.count(key)) return false;
    Utxo u;
    u.txid = key.txid;
    u.vout = key.vout;
    u.amount_sat = amount_sat;
    u.height = confirmed ? height : 0;
    u.confirmed = confirmed;
    u.is_pegout_output = is_pegout_output;
    u.is_coinbase = is_coinbase;
    addr.utxos.emplace(key, std::move(u));
    if (confirmed) {
        addr.confirmed_sat += amount_sat;
        NotifyWatchersLocked(addr, amount_sat);
    } else {
        addr.pending_in_sat += amount_sat;
        BumpRevisionLocked(addr);
    }
    return true;
}

// PromoteUtxo: a previously-pending UTXO has just appeared in a block.
bool PromoteUtxoLocked(Address& addr, const UtxoKey& key, int64_t height) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return false;
    Utxo& u = it->second;
    if (u.confirmed) return false; // already confirmed — idempotent
    u.confirmed = true;
    u.height = height;
    if (u.spent_pending) {
        // utxo was pending-in *and* pending-spent: it never showed up in any
        // visible bucket. Promotion now puts it into pending_out_sat (still
        // pending — the spending tx is in the mempool, awaiting confirmation).
        addr.pending_out_sat += u.amount_sat;
        BumpRevisionLocked(addr);
    } else {
        addr.pending_in_sat -= u.amount_sat;
        addr.confirmed_sat += u.amount_sat;
        NotifyWatchersLocked(addr, u.amount_sat);
    }
    return true;
}

// Inverse of Promote: a confirmed UTXO is being demoted back to pending
// because its block was rolled back. The mempool entry (if any) stays put.
bool DemoteUtxoLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return false;
    Utxo& u = it->second;
    if (!u.confirmed) return false;
    u.confirmed = false;
    u.height = 0;
    if (u.spent_pending) {
        addr.pending_out_sat -= u.amount_sat;
        BumpRevisionLocked(addr);
    } else {
        addr.confirmed_sat -= u.amount_sat;
        addr.pending_in_sat += u.amount_sat;
        NotifyWatchersLocked(addr, -u.amount_sat);
    }
    return true;
}

// SpendUtxo: confirmed-spend recorded by a block.
bool SpendUtxoLocked(Address& addr, const UtxoKey& key, int64_t spent_height) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) {
        SetAddressDesyncLocked(addr, true);
        return false;
    }
    Utxo& u = it->second;
    if (u.spent) return false; // idempotent
    u.spent = true;
    u.spent_height = spent_height;
    int64_t delta = 0;
    if (u.confirmed && u.spent_pending) {
        addr.pending_out_sat -= u.amount_sat;
    } else if (u.confirmed) {
        addr.confirmed_sat -= u.amount_sat;
        delta = -u.amount_sat;
    } else if (!u.confirmed && u.spent_pending) {
        // utxo was hidden (mempool add + mempool spend in the same package)
    } else {
        // !confirmed && !spent_pending — same-block CPFP case
        addr.pending_in_sat -= u.amount_sat;
    }
    u.spent_pending = false;
    u.spent_pending_txid.clear();
    NotifyWatchersLocked(addr, delta);
    return true;
}

// Inverse of SpendUtxo: a previously-confirmed spend is being undone (reorg).
bool UnspendUtxoLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) {
        SetAddressDesyncLocked(addr, true);
        return false;
    }
    Utxo& u = it->second;
    if (!u.spent) return false;
    u.spent = false;
    u.spent_height = 0;
    int64_t delta = 0;
    if (u.confirmed) {
        addr.confirmed_sat += u.amount_sat;
        delta = u.amount_sat;
    } else {
        // CPFP rollback: same-block add+spend, undo the spend half.
        addr.pending_in_sat += u.amount_sat;
    }
    NotifyWatchersLocked(addr, delta);
    return true;
}

// Inverse of AddUtxo (confirmed branch only): drop a confirmed utxo on reorg.
bool RemoveUtxoLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return false;
    Utxo& u = it->second;
    if (u.spent) return false;
    int64_t delta = 0;
    if (u.confirmed && u.spent_pending) {
        addr.pending_out_sat -= u.amount_sat;
    } else if (u.confirmed) {
        addr.confirmed_sat -= u.amount_sat;
        delta = -u.amount_sat;
    } else if (!u.confirmed && u.spent_pending) {
        // hidden bucket — nothing
    } else {
        addr.pending_in_sat -= u.amount_sat;
    }
    addr.utxos.erase(it);
    NotifyWatchersLocked(addr, delta);
    return true;
}

// Pending spend: a mempool tx claims to spend our UTXO.
bool PendingSpendUtxoLocked(Address& addr, const UtxoKey& key, const std::string& spender_txid) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) {
        SetAddressDesyncLocked(addr, true);
        return false;
    }
    Utxo& u = it->second;
    if (u.spent) return false;
    if (u.spent_pending) return false; // first-spender wins; conflicts handled by mempool sync rollback
    u.spent_pending = true;
    u.spent_pending_txid = spender_txid;
    int64_t delta = 0;
    if (u.confirmed) {
        addr.confirmed_sat -= u.amount_sat;
        addr.pending_out_sat += u.amount_sat;
        delta = -u.amount_sat;
    } else {
        addr.pending_in_sat -= u.amount_sat; // hides the utxo from the visible buckets
    }
    NotifyWatchersLocked(addr, delta);
    return true;
}

// Inverse of PendingSpend: a mempool spend has been undone (tx removed from
// mempool without promotion).
bool RollbackPendingSpendLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return false;
    Utxo& u = it->second;
    if (!u.spent_pending) return false;
    u.spent_pending = false;
    u.spent_pending_txid.clear();
    int64_t delta = 0;
    if (u.confirmed) {
        addr.pending_out_sat -= u.amount_sat;
        addr.confirmed_sat += u.amount_sat;
        delta = u.amount_sat;
    } else {
        addr.pending_in_sat += u.amount_sat;
    }
    NotifyWatchersLocked(addr, delta);
    return true;
}

// Drop a pending utxo (both pending-add and pending-spend halves are unwound
// elsewhere). Called from RollbackPendingTx for ADD events.
bool RollbackPendingAddLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return false;
    Utxo& u = it->second;
    if (u.confirmed) return false; // already promoted — chain block owns the drop
    if (u.spent_pending) {
        // hidden bucket — nothing to subtract; the spend tx will be unwound
        // independently by its own mempool removal.
    } else {
        addr.pending_in_sat -= u.amount_sat;
    }
    addr.utxos.erase(it);
    BumpRevisionLocked(addr);
    return true;
}

// GC a fully-buried confirmed-spent utxo when its block leaves the ring.
void GcUtxoLocked(Address& addr, const UtxoKey& key) {
    auto it = addr.utxos.find(key);
    if (it == addr.utxos.end()) return;
    if (!(it->second.spent && it->second.confirmed)) return;
    addr.utxos.erase(it);
    // No notify: balance buckets unchanged.
}

// ---------------------------------------------------------------------------
// MWEB single-writer helpers — keyed by commitment, not script.
// MWEB outputs by construction belong to at most one wallet (trial-decryption
// with master scan_secret), so no shared-watcher fan-out: notify is
// single-target. Mempool MWEB is out of scope for v0; everything we record is
// confirmed.
// ---------------------------------------------------------------------------

// Propagate a confirmed-balance delta on `a` to every wallet watching it.
// Mirrors canonical NotifyWatchersLocked exactly. Pending-in/out changes
// don't go through here — they mutate the address fields directly and
// status JSON aggregates them on demand from bindings.
void NotifyMwebWatchersLocked(MwebAddress& a, int64_t delta) {
    for (OyoWalletImpl* w : a.watchers) {
        w->mweb_balance_sat += delta;
        w->balance_sat      += delta;
        w->revision++;
    }
}

// Bump every watcher's revision without touching balance — for changes
// that move pending counters or address-level flags (desync) without
// affecting confirmed.
void BumpMwebRevisionLocked(MwebAddress& a) {
    NotifyMwebWatchersLocked(a, 0);
}

void SetMwebAddressDesyncLocked(MwebAddress& a, bool desync) {
    if (a.desync == desync) return;
    a.desync = desync;
    BumpMwebRevisionLocked(a);
}

bool AddMwebUtxoLocked(OyoChainImpl& c,
                       MwebAddress& a,
                       const std::array<uint8_t, 33>& commitment,
                       const std::array<uint8_t, 32>& output_id,
                       const std::array<uint8_t, 32>& shared_secret,
                       int64_t amount_sat,
                       int64_t height) {
    if (a.utxos.count(commitment)) return false;  // idempotent

    MwebUtxo u;
    u.commitment    = commitment;
    u.output_id     = output_id;
    u.shared_secret = shared_secret;
    u.amount_sat    = amount_sat;
    u.height        = height;
    u.confirmed     = true;
    a.utxos.emplace(commitment, std::move(u));
    a.confirmed_sat += amount_sat;

    MwebUtxoEntry entry;
    entry.owner_addr    = &a;
    entry.amount_sat    = amount_sat;
    entry.height        = height;
    entry.output_id     = output_id;
    entry.shared_secret = shared_secret;
    c.mweb_utxo_index[commitment] = entry;

    NotifyMwebWatchersLocked(a, amount_sat);
    return true;
}

// History-only insert: record an output the wallet received but that the mirror
// already shows spent (spent before this wallet bootstrapped, recovered from the
// full journal). It is NOT live: it can never be spent again and is past the
// in-memory rollback horizon, so it is deliberately kept OUT of confirmed_sat and
// out of the chain spend index (mweb_utxo_index) and triggers no watcher notify —
// it exists purely so the explorer can show the address's complete receive history.
void AddHistoricalSpentMwebUtxoLocked(MwebAddress& a,
                                      const std::array<uint8_t, 33>& commitment,
                                      const std::array<uint8_t, 32>& output_id,
                                      const std::array<uint8_t, 32>& shared_secret,
                                      int64_t amount_sat,
                                      int64_t height,
                                      int64_t spent_height) {
    if (a.utxos.count(commitment)) return;  // idempotent; live entry wins
    MwebUtxo u;
    u.commitment    = commitment;
    u.output_id     = output_id;
    u.shared_secret = shared_secret;
    u.amount_sat    = amount_sat;
    u.height        = height;
    u.confirmed     = true;
    u.spent         = true;
    u.spent_height  = spent_height;
    a.utxos.emplace(commitment, std::move(u));
}

bool SpendMwebUtxoLocked(OyoChainImpl& c,
                         const std::array<uint8_t, 33>& commitment,
                         int64_t spent_height) {
    auto it = c.mweb_utxo_index.find(commitment);
    if (it == c.mweb_utxo_index.end()) return false;  // not ours, ignore
    MwebAddress* a = it->second.owner_addr;
    if (!a) {
        c.mweb_utxo_index.erase(it);
        return false;
    }
    auto uit = a->utxos.find(commitment);
    if (uit == a->utxos.end()) {
        SetMwebAddressDesyncLocked(*a, true);
        c.mweb_utxo_index.erase(it);
        return false;
    }
    MwebUtxo& u = uit->second;
    if (u.spent) return false;  // idempotent
    u.spent        = true;
    u.spent_height = spent_height;
    int64_t delta = 0;
    if (u.confirmed) {
        a->confirmed_sat -= u.amount_sat;
        delta = -u.amount_sat;
    }
    NotifyMwebWatchersLocked(*a, delta);
    // Keep entry in chain.mweb_utxo_index until block leaves the ring window
    // (mirrors P2WPKH spent-keep-until-GC). Allows reorg to UnspendMweb the
    // utxo without losing owner attribution.
    return true;
}

bool UnspendMwebUtxoLocked(OyoChainImpl& c,
                           const std::array<uint8_t, 33>& commitment) {
    auto it = c.mweb_utxo_index.find(commitment);
    if (it == c.mweb_utxo_index.end()) return false;
    MwebAddress* a = it->second.owner_addr;
    if (!a) return false;
    auto uit = a->utxos.find(commitment);
    if (uit == a->utxos.end()) return false;
    MwebUtxo& u = uit->second;
    if (!u.spent) return false;
    u.spent        = false;
    u.spent_height = 0;
    int64_t delta = 0;
    if (u.confirmed) {
        a->confirmed_sat += u.amount_sat;
        delta = u.amount_sat;
    }
    NotifyMwebWatchersLocked(*a, delta);
    return true;
}

// Inverse of AddMwebUtxoLocked: removes a recently-added utxo on rollback,
// before it ever became spent.
bool RemoveMwebUtxoLocked(OyoChainImpl& c, const std::array<uint8_t, 33>& commitment) {
    auto it = c.mweb_utxo_index.find(commitment);
    if (it == c.mweb_utxo_index.end()) return false;
    MwebAddress* a = it->second.owner_addr;
    if (!a) {
        c.mweb_utxo_index.erase(it);
        return false;
    }
    auto uit = a->utxos.find(commitment);
    if (uit == a->utxos.end()) {
        c.mweb_utxo_index.erase(it);
        return false;
    }
    MwebUtxo& u = uit->second;
    int64_t delta = 0;
    if (u.confirmed && !u.spent) {
        a->confirmed_sat -= u.amount_sat;
        delta = -u.amount_sat;
    }
    a->utxos.erase(uit);
    c.mweb_utxo_index.erase(it);
    NotifyMwebWatchersLocked(*a, delta);
    return true;
}

void InvalidateLoadedWalletMirrorStateLocked(OyoChainImpl& c) {
    for (auto& kv : c.addresses) {
        Address& a = *kv.second;
        std::vector<UtxoKey> to_remove;
        int64_t delta = 0;
        bool changed = false;
        for (const auto& ukv : a.utxos) {
            const Utxo& u = ukv.second;
            if (!u.confirmed) continue;
            if (!u.spent) {
                if (u.spent_pending) {
                    a.pending_out_sat -= u.amount_sat;
                } else {
                    a.confirmed_sat -= u.amount_sat;
                    delta -= u.amount_sat;
                }
            }
            to_remove.push_back(ukv.first);
            changed = true;
        }
        for (const auto& k : to_remove) a.utxos.erase(k);
        if (changed) NotifyWatchersLocked(a, delta);
        SetAddressDesyncLocked(a, true);
    }

    c.mweb_utxo_index.clear();
    for (auto& kv : c.mweb_addresses) {
        MwebAddress& a = *kv.second;
        std::vector<std::array<uint8_t, 33>> to_remove;
        int64_t delta = 0;
        bool changed = false;
        for (const auto& ukv : a.utxos) {
            const MwebUtxo& u = ukv.second;
            if (!u.confirmed) continue;
            if (!u.spent) {
                if (c.mweb_mempool_pending_out.erase(ukv.first) != 0) {
                    a.pending_out_sat -= u.amount_sat;
                } else {
                    a.confirmed_sat -= u.amount_sat;
                    delta -= u.amount_sat;
                }
            }
            to_remove.push_back(ukv.first);
            changed = true;
        }
        for (const auto& k : to_remove) a.utxos.erase(k);
        if (changed) NotifyMwebWatchersLocked(a, delta);
        SetMwebAddressDesyncLocked(a, true);
    }
}

// Adds a mempool-pending MWEB output. Bumps the address's pending_in_sat;
// status JSON aggregates per-wallet pending across bindings on demand.
// Idempotent on duplicate commitment.
bool AddPendingMwebUtxoLocked(OyoChainImpl& c,
                              MwebAddress& a,
                              const std::array<uint8_t, 33>& commitment,
                              const std::array<uint8_t, 32>& output_id,
                              const std::array<uint8_t, 32>& shared_secret,
                              int64_t amount_sat) {
    if (c.mweb_mempool_pending.count(commitment)) return false;

    MwebUtxoEntry entry;
    entry.owner_addr    = &a;
    entry.amount_sat    = amount_sat;
    entry.height        = 0;
    entry.output_id     = output_id;
    entry.shared_secret = shared_secret;
    c.mweb_mempool_pending[commitment] = entry;

    a.pending_in_sat += amount_sat;
    BumpMwebRevisionLocked(a);
    return true;
}

// Inverse of AddPendingMwebUtxoLocked: tx left mempool without confirming.
bool RemovePendingMwebUtxoLocked(OyoChainImpl& c,
                                  const std::array<uint8_t, 33>& commitment) {
    auto it = c.mweb_mempool_pending.find(commitment);
    if (it == c.mweb_mempool_pending.end()) return false;
    MwebAddress* a = it->second.owner_addr;
    int64_t amt = it->second.amount_sat;
    if (a) {
        a->pending_in_sat -= amt;
        BumpMwebRevisionLocked(*a);
    }
    c.mweb_mempool_pending.erase(it);
    return true;
}

// Called from ApplyMwebEventLocked(ADD) before the confirmed AddMwebUtxoLocked
// runs: if the same commitment was tracked as pending, decrement pending caches
// (the confirmed Add will increment confirmed_sat). Net zero change to wallet's
// total balance during the promote — pending_in shrinks, confirmed grows.
void PromoteMwebPendingOnConfirmLocked(OyoChainImpl& c,
                                       const std::array<uint8_t, 33>& commitment) {
    RemovePendingMwebUtxoLocked(c, commitment);
    c.mweb_mempool_seen_not_ours.erase(commitment);
}

// Adds a pending-OUT marker on a confirmed UTXO that's being spent by a
// mempool tx. Decrements `confirmed_sat` from the visible balance and tracks
// the amount under `pending_out_sat`. Idempotent.
bool AddPendingMwebSpendLocked(OyoChainImpl& c,
                                const std::array<uint8_t, 33>& commitment) {
    if (!c.mweb_mempool_pending_out.insert(commitment).second) return false;
    auto it = c.mweb_utxo_index.find(commitment);
    if (it == c.mweb_utxo_index.end()) return true;  // not ours, ignore
    MwebAddress* a = it->second.owner_addr;
    int64_t amt = it->second.amount_sat;
    if (a) {
        a->confirmed_sat   -= amt;
        a->pending_out_sat += amt;
        // Confirmed shrinks → notify with -amt; pending_out delta is
        // already on the address itself, status JSON sums it.
        NotifyMwebWatchersLocked(*a, -amt);
    }
    return true;
}

bool RemovePendingMwebSpendLocked(OyoChainImpl& c,
                                   const std::array<uint8_t, 33>& commitment) {
    if (c.mweb_mempool_pending_out.erase(commitment) == 0) return false;
    auto it = c.mweb_utxo_index.find(commitment);
    if (it == c.mweb_utxo_index.end()) return true;
    MwebAddress* a = it->second.owner_addr;
    int64_t amt = it->second.amount_sat;
    if (a) {
        a->confirmed_sat   += amt;
        a->pending_out_sat -= amt;
        NotifyMwebWatchersLocked(*a, amt);
    }
    return true;
}

// GC a fully-buried confirmed-spent MWEB utxo when its block leaves the ring.
void GcMwebUtxoLocked(OyoChainImpl& c, const std::array<uint8_t, 33>& commitment) {
    // Free the chain-level spend index (the output is spent past the rollback
    // window — it can never be spent again or reorged), but KEEP the spent utxo
    // in its address's map: that map is the address's receive history, surfaced
    // by the explorer. Symmetric with the regular mirror, which also keeps spent
    // rows as a full journal. Spend-selection and the balance both skip spent
    // entries, so retaining them is inert for everything except history.
    c.mweb_utxo_index.erase(commitment);
}

// ---------------------------------------------------------------------------
// Address registration / binding
// ---------------------------------------------------------------------------

Address* GetOrCreateAddressLocked(OyoChainImpl& c,
                                  const std::string& script_hex,
                                  const std::string& kind,
                                  const std::string& addr_str) {
    auto it = c.addresses.find(script_hex);
    if (it != c.addresses.end()) return it->second.get();
    auto a = std::make_unique<Address>();
    a->script_hex = script_hex;
    a->kind = kind;
    a->address = addr_str;
    Address* p = a.get();
    c.addresses.emplace(script_hex, std::move(a));
    // A previously-cached "not ours" mempool tx might actually touch this
    // new address — wipe the seen cache so the next mempool sync rechecks
    // every txid against the updated script index.
    c.mempool_seen_not_ours.clear();
    return p;
}

// After attaching a brand-new Address to the chain, walk the mempool ring
// and replay any already-tracked PendingTx events that touch this script.
// Without this, an address registered after a mempool tx was first seen
// would never receive its share of pending in/out (mempool sync diff
// only re-fetches truly new tx-ids, not previously-known ones).
void ReplayMempoolForAddressLocked(OyoChainImpl& c, Address& addr) {
    for (auto& kv : c.mempool) {
        const PendingTx& tx = kv.second;
        if (tx.promoted) continue; // already converted to confirmed via block apply
        for (const auto& e : tx.events) {
            if (e.script_hex != addr.script_hex) continue;
            if (e.kind == EventKind::ADD) {
                AddUtxoLocked(addr, e.key, e.amount_sat, 0, /*confirmed=*/false);
            } else {
                PendingSpendUtxoLocked(addr, e.key, tx.txid);
            }
        }
    }
}

void AttachWatcherLocked(Address& addr, OyoWalletImpl* w) {
    for (OyoWalletImpl* x : addr.watchers) if (x == w) return;
    addr.watchers.push_back(w);
    w->balance_sat += addr.confirmed_sat;
    w->revision++;
}

void DetachWatcherLocked(OyoChainImpl& c, Address& addr, OyoWalletImpl* w) {
    auto it = std::find(addr.watchers.begin(), addr.watchers.end(), w);
    if (it == addr.watchers.end()) return;
    addr.watchers.erase(it);
    w->balance_sat -= addr.confirmed_sat;
    w->revision++;
    if (addr.watchers.empty()) {
        c.addresses.erase(addr.script_hex);
    }
}

// Allocates one P2WPKH binding at the next sequential index and registers
// it with the chain. The single canonical derivation chain ("oyo_v1") is
// used for Regular and Universal alike — both store their free-form seed
// string in seed_bytes. Returns the new binding index on success, or -1 if
// the wallet has no p2wpkh side or derivation fails. Used by both the open
// path (loop 0..N-1) and the runtime allocate-on-demand path
// (oyo_external_wallet_new_p2wpkh_address + change-address selection).
//
// `runtime_alloc`: when true, marks a freshly-created Address as
// not-desync (no historical activity could exist for an HD index that's
// never been used before). Pre-allocate at open passes false so the
// initial rescan path works as before.
// Generalized canonical-binding allocator. kind ∈ {"p2wpkh","p2pkh",
// "p2sh-p2wpkh"} — all three derive from the SAME oyo_v1 key chain; only the
// scriptPubKey / address encoding differs (P2.1: legacy + nested SegWit), so
// they share one keychain and one coin-selection set. P2SH-P2WPKH wraps a
// P2WPKH redeem script. Detection is automatic — addresses are keyed by their
// scriptPubKey hex, which the block-walk matches against vout scripts.
int AllocateCanonicalBindingLocked(OyoChainImpl& chain, OyoWalletImpl& w,
                                   const std::string& kind,
                                   bool runtime_alloc = false) {
    if (!w.has_p2wpkh) return -1;
    if (w.address_kind != "p2wpkh") return -1;
    if (kind != "p2wpkh" && kind != "p2pkh" && kind != "p2sh-p2wpkh") return -1;
    const int idx = static_cast<int>(w.bindings.size());
    const std::string net = chain.network.empty() ? std::string("regtest") : chain.network;
    const std::string hrp = NetworkHrp(net);

    std::vector<uint8_t> pub_v;
    if (w.seed_type == "oyo_v1") {
        if (!DeriveOyoV1Pubkey(w.seed_bytes, net, idx, pub_v)) return -1;
    } else {
        return -1;  // watch / mweb-only / unsupported seed kind
    }

    uint160 h160 = Hash160(pub_v);
    std::string script, addr_str;
    if (kind == "p2wpkh") {
        script   = "0014" + ToHexLower(h160.begin(), 20);
        addr_str = EncodeP2WPKH(hrp, h160.begin(), 20);
    } else if (kind == "p2pkh") {
        CScript spk = GetScriptForDestination(PKHash(h160));        // 76a914<h160>88ac
        script   = HexStr(spk);
        addr_str = EncodeDestination(PKHash(h160));
    } else {  // p2sh-p2wpkh: P2SH wrapping the P2WPKH redeem script
        CScript redeem = GetScriptForDestination(WitnessV0KeyHash(h160));  // 0014<h160>
        CScript spk    = GetScriptForDestination(ScriptHash(redeem));      // a914<h160(redeem)>87
        script   = HexStr(spk);
        addr_str = EncodeDestination(ScriptHash(redeem));
    }
    if (script.empty() || addr_str.empty()) return -1;
    Address* addr = GetOrCreateAddressLocked(chain, script, kind, addr_str);
    // Runtime allocate-on-demand: a freshly-derived HD index has never
    // been seen on chain → no historical activity to miss → not desync.
    // Skipped during initial pre-allocation (`runtime_alloc=false`) where
    // the user may have imported a known seed and a subsequent rescan
    // is the right place to clear desync.
    if (runtime_alloc && addr->utxos.empty() && addr->confirmed_sat == 0 &&
        addr->pending_in_sat == 0 && addr->pending_out_sat == 0) {
        addr->desync = false;
    }
    auto b = std::make_unique<Binding>();
    b->addr  = addr;
    b->index = idx;
    w.bindings.push_back(std::move(b));
    AttachWatcherLocked(*addr, &w);
    if (idx >= w.address_count) w.address_count = idx + 1;
    w.revision++;
    return idx;
}

// P2WPKH wrapper — change allocation + the default canonical address use it.
int AllocateP2wpkhBindingLocked(OyoChainImpl& chain, OyoWalletImpl& w,
                                bool runtime_alloc = false) {
    return AllocateCanonicalBindingLocked(chain, w, "p2wpkh", runtime_alloc);
}

// ---------------------------------------------------------------------------
// Block parsing — fully decodes a getblock-verbosity-2 payload into events.
// ---------------------------------------------------------------------------

// Returns scriptPubKey hex ("0014<h160>") of the address that spent this vin
// for a P2WPKH input. Empty string on failure (coinbase, non-P2WPKH shape, etc).
std::string SpenderScriptP2WPKH(const UniValue& vin) {
    const UniValue& w = vin["txinwitness"];
    if (!w.isArray() || w.size() != 2) return {};
    const UniValue& pub_v = w[1];
    if (!pub_v.isStr()) return {};
    std::vector<uint8_t> pub = HexToBytes(pub_v.get_str());
    if (pub.size() != 33) return {};
    uint160 h = Hash160(pub);
    return "0014" + ToHexLower(h.begin(), 20);
}

// Append BlockEvents extracted from a single tx-shape JSON object (vin/vout +
// txid). Same shape returned by getblock verbosity=2 per tx and by
// getrawtransaction verbose=true at the top level.
//
// is_hogex_tx: when true, vouts beyond vout[0] are peg-out materialisations
// from the MWEB extension's HogEx; vout[0] is the passthrough to the next
// block's HogEx (always non-spendable). Marks ADD events accordingly so
// coin selection can apply PEGOUT_MATURITY (6 blocks).
void ParseTxEvents(const UniValue& tx, std::vector<BlockEvent>& out,
                   bool is_hogex_tx = false) {
    const UniValue& this_txid_v = tx["txid"];
    if (!this_txid_v.isStr()) return;
    std::string this_txid = LowerHex(this_txid_v.get_str());
    const UniValue& vins = tx["vin"];
    // The coinbase is the tx whose first input carries a "coinbase" field;
    // its vouts need COINBASE_MATURITY blocks before they can be spent.
    // Mempool txs never carry the field, so the mempool caller gets false.
    const bool is_coinbase_tx = vins.isArray() && vins.size() >= 1 &&
                                vins[0].isObject() && vins[0].exists("coinbase");
    if (vins.isArray()) {
        for (size_t vi = 0; vi < vins.size(); ++vi) {
            const UniValue& vin = vins[vi];
            if (vin.exists("coinbase")) continue;
            const UniValue& txid_v = vin["txid"];
            const UniValue& vout_v = vin["vout"];
            if (!txid_v.isStr() || !vout_v.isNum()) continue;
            std::string spender = SpenderScriptP2WPKH(vin);
            if (spender.empty()) continue;
            BlockEvent e;
            e.kind = EventKind::SPEND;
            e.key.txid = LowerHex(txid_v.get_str());
            e.key.vout = uint32_t(vout_v.get_int64());
            e.script_hex = std::move(spender);
            e.amount_sat = 0;
            e.event_tx = this_txid;
            out.push_back(std::move(e));
        }
    }
    const UniValue& vouts = tx["vout"];
    if (!vouts.isArray()) return;
    for (size_t oi = 0; oi < vouts.size(); ++oi) {
        const UniValue& vout = vouts[oi];
        const UniValue& n_v = vout["n"];
        uint32_t n_idx = n_v.isNum() ? uint32_t(n_v.get_int64()) : uint32_t(oi);
        const UniValue& spk = vout["scriptPubKey"];
        if (!spk.isObject()) continue;
        const UniValue& hex_v = spk["hex"];
        if (!hex_v.isStr()) continue;
        const UniValue& val = vout["value"];
        int64_t sat = 0;
        if (val.isNum()) sat = LtcStringToSat(val.getValStr());
        else if (val.isStr()) sat = LtcStringToSat(val.get_str());
        BlockEvent e;
        e.kind = EventKind::ADD;
        e.key.txid = this_txid;
        e.key.vout = n_idx;
        e.script_hex = LowerHex(hex_v.get_str());
        e.amount_sat = sat;
        e.event_tx = this_txid;
        // HogEx vout[0] is the passthrough (not a peg-out); vout[1..] are
        // the peg-out materialisations subject to PEGOUT_MATURITY.
        e.is_pegout_output = is_hogex_tx && n_idx > 0;
        e.is_coinbase = is_coinbase_tx;
        out.push_back(std::move(e));
    }
}

// Detects whether a tx is the block's HogEx by inspecting its vout[0]
// shape: a HogEx's first vout is the passthrough kernel (carried over
// to the next block), with scriptPubKey = OP_8 (0x58) || OP_PUSHBYTES_32
// (0x20) || <32-byte kernel hash> = 34 bytes total. No regular send
// tx produces this exact shape — it's distinctive enough to flag the
// HogEx without false positives. The naive "last tx in MWEB-active
// block" heuristic mis-fires when a regtest block has only [coinbase,
// regular_tx] (no actual HogEx since there was no MWEB activity), and
// then marks the regular_tx's vouts as immature peg-outs.
bool TxIsHogEx(const UniValue& tx) {
    const UniValue& vout = tx["vout"];
    if (!vout.isArray() || vout.size() == 0) return false;
    const UniValue& spk = vout[0]["scriptPubKey"];
    if (!spk.isObject()) return false;
    const UniValue& hex_v = spk["hex"];
    if (!hex_v.isStr()) return false;
    const std::string& s = hex_v.get_str();
    // 34 bytes = 68 hex chars; prefix "5820" = OP_8 + 32-byte push.
    return s.size() == 68 && (s[0] == '5' && s[1] == '8') &&
                              (s[2] == '2' && s[3] == '0');
}

std::vector<BlockEvent> ParseBlockEvents(const UniValue& block) {
    std::vector<BlockEvent> out;
    const UniValue& txs = block["tx"];
    if (!txs.isArray()) return out;
    // HogEx detection: a HogEx tx is always the last tx in an MWEB-
    // extended block (primitives/block.cpp:44, mweb/mweb_node.cpp:60),
    // BUT not every block has one — a regtest block with no MWEB
    // activity contains [coinbase, regular_tx] and the regular_tx
    // would otherwise be misidentified as HogEx, marking its vouts
    // as immature peg-outs. Validate the structural shape (vout[0] =
    // OP_8 + 32-byte kernel-hash push) to avoid the false positive.
    const bool has_mweb = block["mweb"].isObject();
    size_t hogex_idx = SIZE_MAX;
    if (has_mweb && txs.size() >= 2 && TxIsHogEx(txs[txs.size() - 1])) {
        hogex_idx = txs.size() - 1;
    }
    for (size_t ti = 0; ti < txs.size(); ++ti) {
        ParseTxEvents(txs[ti], out, /*is_hogex_tx=*/(ti == hogex_idx));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Regular UTXO mirror feed + diff-oracle (transition machinery)
// ---------------------------------------------------------------------------

// 36-byte outpoint key: txid bytes (decoded from the node's hex, i.e. display
// byte order — consistent for both the creating vout and the spending vin since
// both come from the same getblock JSON) followed by vout as 4 LE bytes.
// Identity-only; the byte order is irrelevant as long as it is consistent.
std::vector<uint8_t> RegularOutpointKey(const std::string& txid_hex, uint32_t vout) {
    std::vector<uint8_t> k = HexToBytes(txid_hex);
    k.resize(32);   // defensive: malformed txid → zero-padded, still a stable key
    k.push_back(static_cast<uint8_t>(vout & 0xff));
    k.push_back(static_cast<uint8_t>((vout >> 8) & 0xff));
    k.push_back(static_cast<uint8_t>((vout >> 16) & 0xff));
    k.push_back(static_cast<uint8_t>((vout >> 24) & 0xff));
    return k;
}

int64_t MirrorEventId(int64_t height, int64_t ordinal, int64_t flags = 0) {
    return (height << 32) | ((ordinal & 0xFFFFFF) << 8) | (flags & 0xFF);
}

// Feed a getblock-verbosity-2 block into the regular event-log. Two passes give
// each event a packed monotonic id: pass 1 appends a spend event
// per non-coinbase input (BLIND — the spent output's row was recorded by an
// earlier block, no lookup), pass 2 appends an output event per vout with its
// raw script blob (HogEx vout[1..] get the pegout flag, the coinbase's vouts
// the coinbase flag). Caller wraps this in the per-block mirror transactions.
void FeedRegularMirrorLocked(OyoChainImpl& c, const UniValue& block, int64_t height) {
    if (!c.regular_mirror) return;
    const UniValue& txs = block["tx"];
    if (!txs.isArray()) return;
    // Reorg-only journals (o/s) are written only for blocks within the reorg
    // window of the node tip; deeper-buried blocks are final and would only be
    // pruned, so we never write them (keeps the bootstrap a pure append, no
    // delete/compaction churn). The address/spent indexes (a/x) are always written.
    const bool write_journal =
        c.node_tip_hint < 0 || height >= c.node_tip_hint - c.max_reorg_depth;
    const bool has_mweb = block["mweb"].isObject();
    size_t hogex_idx = SIZE_MAX;
    if (has_mweb && txs.size() >= 2 && TxIsHogEx(txs[txs.size() - 1])) {
        hogex_idx = txs.size() - 1;
    }
    int64_t idx = 0;   // event index within the block: spends first, then outputs

    // Pass 1 — spends: every non-coinbase input of every tx.
    for (size_t ti = 0; ti < txs.size(); ++ti) {
        const UniValue& vins = txs[ti]["vin"];
        if (!vins.isArray()) continue;
        for (size_t vi = 0; vi < vins.size(); ++vi) {
            const UniValue& vin = vins[vi];
            if (vin.exists("coinbase")) continue;
            const UniValue& txid_v = vin["txid"];
            const UniValue& vout_v = vin["vout"];
            if (!txid_v.isStr() || !vout_v.isNum()) continue;
            c.regular_mirror->AppendSpend(
                MirrorEventId(height, idx++),
                RegularOutpointKey(LowerHex(txid_v.get_str()),
                                   uint32_t(vout_v.get_int64())),
                write_journal);
            c.mirror_delta.regular_spends++;
        }
    }
    // Pass 2 — outputs: every vout of every tx.
    for (size_t ti = 0; ti < txs.size(); ++ti) {
        const UniValue& tx = txs[ti];
        const bool is_hogex = (ti == hogex_idx);
        // The coinbase is the (only) tx whose first input carries a "coinbase"
        // field; its vouts need COINBASE_MATURITY blocks before they spend.
        const UniValue& vins0 = tx["vin"];
        const bool is_coinbase = vins0.isArray() && vins0.size() >= 1 &&
                                 vins0[0].isObject() && vins0[0].exists("coinbase");
        const UniValue& vouts = tx["vout"];
        if (!vouts.isArray()) continue;
        const UniValue& txid_v = tx["txid"];
        std::string txid = txid_v.isStr() ? LowerHex(txid_v.get_str()) : std::string();
        for (size_t oi = 0; oi < vouts.size(); ++oi) {
            const UniValue& vout = vouts[oi];
            const UniValue& n_v = vout["n"];
            uint32_t n_idx = n_v.isNum() ? uint32_t(n_v.get_int64()) : uint32_t(oi);
            const UniValue& spk = vout["scriptPubKey"];
            if (!spk.isObject()) continue;
            const UniValue& hex_v = spk["hex"];
            if (!hex_v.isStr()) continue;
            const UniValue& val = vout["value"];
            int64_t sat = 0;
            if (val.isNum()) sat = LtcStringToSat(val.getValStr());
            else if (val.isStr()) sat = LtcStringToSat(val.get_str());
            int64_t flags = 0;
            if (is_coinbase)            flags |= RegularMirror::kFlagCoinbase;
            if (is_hogex && n_idx > 0)  flags |= RegularMirror::kFlagPegOut;
            std::vector<uint8_t> script = HexToBytes(hex_v.get_str());
            c.regular_mirror->AppendOutput(MirrorEventId(height, idx++, flags),
                                           RegularOutpointKey(txid, n_idx),
                                           script, sat, write_journal);
            c.mirror_delta.regular_outputs++;
        }
    }
}


// --- transient-watcher mempool view maintenance ---------------------------
// Record one mempool tx (watched or not) into the per-tx mempool view: its
// outputs (script, amount, vout) and non-coinbase inputs (prevout). No owner
// resolution, no per-script aggregate, no outpoint lookup — pending is derived
// per request in oyo_chain_address_status. Idempotent on txid.
void IndexMempoolTxLocked(OyoChainImpl& c, const UniValue& tx, const std::string& txid) {
    if (!c.regular_mirror) return;            // canonical mempool view only
    if (c.mempool_view.count(txid)) return;   // already recorded
    MempoolTxView view;
    view.txid = txid;

    const UniValue& vouts = tx["vout"];
    if (vouts.isArray()) {
        for (size_t oi = 0; oi < vouts.size(); ++oi) {
            const UniValue& vout = vouts[oi];
            const UniValue& n_v = vout["n"];
            uint32_t n_idx = n_v.isNum() ? uint32_t(n_v.get_int64()) : uint32_t(oi);
            const UniValue& spk = vout["scriptPubKey"];
            if (!spk.isObject()) continue;
            const UniValue& hex_v = spk["hex"];
            if (!hex_v.isStr()) continue;
            const UniValue& val = vout["value"];
            int64_t sat = 0;
            if (val.isNum()) sat = LtcStringToSat(val.getValStr());
            else if (val.isStr()) sat = LtcStringToSat(val.get_str());
            view.outs.push_back({n_idx, LowerHex(hex_v.get_str()), sat});
        }
    }
    const UniValue& vins = tx["vin"];
    if (vins.isArray()) {
        for (size_t vi = 0; vi < vins.size(); ++vi) {
            const UniValue& vin = vins[vi];
            if (vin.exists("coinbase")) continue;
            const UniValue& txid_v = vin["txid"];
            const UniValue& vout_v = vin["vout"];
            if (!txid_v.isStr() || !vout_v.isNum()) continue;
            view.ins.push_back({LowerHex(txid_v.get_str()), uint32_t(vout_v.get_int64())});
        }
    }
    c.mempool_view.emplace(txid, std::move(view));
}

void UnindexMempoolTxLocked(OyoChainImpl& c, const std::string& txid) {
    c.mempool_view.erase(txid);
}

// ---------------------------------------------------------------------------
// Apply / rollback / GC
// ---------------------------------------------------------------------------

// Apply a block event, with mempool-promotion semantics: if our mempool
// already tracks the same tx, mark the PendingTx promoted so the next
// mempool sync diff doesn't try to roll its events back as a "removed".
void ApplyEventLocked(OyoChainImpl& c, const BlockEvent& e, int64_t height) {
    auto it = c.addresses.find(e.script_hex);
    if (it == c.addresses.end()) return;
    Address& addr = *it->second;
    if (e.kind == EventKind::ADD) {
        auto uit = addr.utxos.find(e.key);
        if (uit != addr.utxos.end()) {
            if (!uit->second.confirmed) {
                PromoteUtxoLocked(addr, e.key, height);
            }
            // Carry the peg-out marker into the existing utxo. Two
            // cases this fixes:
            //  - Was pending (just promoted) — block apply is the
            //    first place we know it's a peg-out (mempool entries
            //    don't include block-context).
            //  - Was already confirmed via a fresh rescan that ran
            //    BEFORE chain/sync had block-walked the funding
            //    block, so AddUtxoLocked saved is_pegout_output=false.
            //    Without this OR-update, the wallet would happily
            //    pick the utxo and the node would reject the spend
            //    with "premature spend of pegout".
            uit->second.is_pegout_output =
                uit->second.is_pegout_output || e.is_pegout_output;
            // Same carry for the coinbase marker (same rescan-before-sync
            // case — a fresh rescan saved the utxo before block apply knew
            // it came from a coinbase).
            uit->second.is_coinbase =
                uit->second.is_coinbase || e.is_coinbase;
        } else {
            AddUtxoLocked(addr, e.key, e.amount_sat, height,
                           /*confirmed=*/true, e.is_pegout_output,
                           e.is_coinbase);
        }
    } else {
        SpendUtxoLocked(addr, e.key, height);
    }
    // Tag any matching mempool entry as promoted so it's not rolled back
    // when mempool diff shows it as "removed from node mempool".
    if (!e.event_tx.empty()) {
        auto mit = c.mempool.find(e.event_tx);
        if (mit != c.mempool.end()) mit->second.promoted = true;
    }
}

void RollbackEventLocked(OyoChainImpl& c, const BlockEvent& e) {
    auto it = c.addresses.find(e.script_hex);
    if (it == c.addresses.end()) return;
    Address& addr = *it->second;
    if (e.kind == EventKind::ADD) {
        // If the mempool still tracks this tx, demote rather than remove —
        // the utxo lives on as pending.
        auto mit = e.event_tx.empty() ? c.mempool.end() : c.mempool.find(e.event_tx);
        auto uit = addr.utxos.find(e.key);
        if (mit != c.mempool.end() && uit != addr.utxos.end() && uit->second.confirmed) {
            DemoteUtxoLocked(addr, e.key);
            mit->second.promoted = false;
        } else {
            RemoveUtxoLocked(addr, e.key);
        }
    } else {
        UnspendUtxoLocked(addr, e.key);
        // If the mempool still tracks the spending tx, restore the pending
        // spend mark so the utxo stays in pending_out.
        if (!e.event_tx.empty()) {
            auto mit = c.mempool.find(e.event_tx);
            if (mit != c.mempool.end()) {
                PendingSpendUtxoLocked(addr, e.key, e.event_tx);
                mit->second.promoted = false;
            }
        }
    }
}

// Mempool tx apply / rollback. Walks tx.events, dispatches per kind.
void ApplyPendingTxLocked(OyoChainImpl& c, PendingTx&& tx) {
    if (c.mempool.count(tx.txid)) return; // already tracked
    for (const auto& e : tx.events) {
        auto it = c.addresses.find(e.script_hex);
        if (it == c.addresses.end()) continue;
        Address& addr = *it->second;
        if (e.kind == EventKind::ADD) {
            AddUtxoLocked(addr, e.key, e.amount_sat, 0, /*confirmed=*/false);
        } else {
            PendingSpendUtxoLocked(addr, e.key, tx.txid);
        }
    }
    std::string id = tx.txid;
    c.mempool.emplace(std::move(id), std::move(tx));
}

void RollbackPendingTxLocked(OyoChainImpl& c, const std::string& txid) {
    auto it = c.mempool.find(txid);
    if (it == c.mempool.end()) return;
    if (it->second.promoted) {
        // already converted into confirmed via block apply — drop the record
        // without touching addresses (block apply already updated them).
        c.mempool.erase(it);
        return;
    }
    PendingTx tx = std::move(it->second);
    c.mempool.erase(it);
    for (auto eit = tx.events.rbegin(); eit != tx.events.rend(); ++eit) {
        const BlockEvent& e = *eit;
        auto ait = c.addresses.find(e.script_hex);
        if (ait == c.addresses.end()) continue;
        Address& addr = *ait->second;
        if (e.kind == EventKind::ADD) {
            RollbackPendingAddLocked(addr, e.key);
        } else {
            RollbackPendingSpendLocked(addr, e.key);
        }
    }
}

// ---------------------------------------------------------------------------
// MWEB block-events: parse the .mweb section of a getblock-verbosity-2
// payload and convert into MwebBlockEvent records owned by the chain.
// Outputs become ADD events only when one of our registered keychains
// rewinds them. Inputs become SPEND events only when their commitment is in
// our chain.mweb_utxo_index — foreign spends are silently ignored, matching
// the P2WPKH "spend of unknown script_hex" behavior.
// ---------------------------------------------------------------------------

std::vector<MwebBlockEvent> ParseMwebBlockEventsLocked(OyoChainImpl& c,
                                                       const UniValue& block) {
    std::vector<MwebBlockEvent> out;
    if (c.mweb_keychains.empty() && c.mweb_utxo_index.empty()) {
        // No MWEB consumers attached and no prior state — cheap exit.
        return out;
    }
    const UniValue& mweb = block["mweb"];
    if (!mweb.isObject()) return out;

    const UniValue& outputs = mweb["outputs"];
    if (outputs.isArray()) {
        for (size_t i = 0; i < outputs.size(); ++i) {
            const UniValue& o = outputs[i];
            if (!o.isObject()) continue;
            const std::string output_id_hex = o["output_id"].isStr() ? o["output_id"].get_str() : std::string();
            const std::string commit_hex    = o["commit"].isStr()    ? o["commit"].get_str()    : std::string();
            const std::string ko_hex        = o["receiver_pubkey"].isStr() ? o["receiver_pubkey"].get_str() : std::string();
            const std::string msg_hex       = o["message"].isStr()   ? o["message"].get_str()   : std::string();
            if (commit_hex.size() != 66 || ko_hex.size() != 66 || msg_hex.empty()) continue;

            // Hex → bytes once, at the JSON boundary.
            std::array<uint8_t, 33> commit_b;
            std::array<uint8_t, 33> ko_b;
            std::array<uint8_t, 32> oid_b{};
            {
                auto cv = ParseHex(commit_hex);
                auto kv = ParseHex(ko_hex);
                if (cv.size() != 33 || kv.size() != 33) continue;
                std::memcpy(commit_b.data(), cv.data(), 33);
                std::memcpy(ko_b.data(),     kv.data(), 33);
                if (output_id_hex.size() == 64) {
                    auto ov = ParseHex(output_id_hex);
                    if (ov.size() == 32) std::memcpy(oid_b.data(), ov.data(), 32);
                }
            }
            std::vector<uint8_t> msg_b = ParseHex(msg_hex);

            // Try-rewind against each registered keychain. First match wins;
            // an output's B_i belongs to at most one master spend key, so
            // at most one (deduped) keychain can decode it. Multiple wallets
            // sharing a seed share the same keychain entry — they all see
            // the resulting event through the chain-level MwebAddress.
            for (const auto& kc_ptr : c.mweb_keychains) {
                MwebKeychain& kc = *kc_ptr;
                if (kc.spend_pubkey_index.empty()) continue;  // no addresses allocated yet
                auto rr = oyoltc::mweb::TryRewindOutput(
                    kc.scan_secret, kc.spend_pubkey_index,
                    commit_b, ko_b, msg_b);
                if (!rr.matched || !rr.matched_address) continue;

                MwebBlockEvent e;
                e.kind          = MwebEventKind::ADD;
                e.commitment    = commit_b;
                e.output_id     = oid_b;
                e.shared_secret = rr.shared_secret;
                e.amount_sat    = static_cast<int64_t>(rr.amount);
                e.owner_addr    = rr.matched_address;
                out.push_back(std::move(e));
                break;
            }
        }
    }

    const UniValue& inputs = mweb["inputs"];
    if (inputs.isArray()) {
        for (size_t i = 0; i < inputs.size(); ++i) {
            const UniValue& in = inputs[i];
            if (!in.isObject()) continue;
            const std::string commit_hex = in["commit"].isStr() ? in["commit"].get_str() : std::string();
            if (commit_hex.size() != 66) continue;
            std::array<uint8_t, 33> commit_b;
            {
                auto cv = ParseHex(commit_hex);
                if (cv.size() != 33) continue;
                std::memcpy(commit_b.data(), cv.data(), 33);
            }

            auto it = c.mweb_utxo_index.find(commit_b);
            if (it == c.mweb_utxo_index.end()) continue;  // not ours, ignore

            MwebBlockEvent e;
            e.kind       = MwebEventKind::SPEND;
            e.commitment = commit_b;
            e.owner_addr = it->second.owner_addr;
            out.push_back(std::move(e));
        }
    }
    return out;
}

void ApplyMwebEventLocked(OyoChainImpl& c,
                          const MwebBlockEvent& e,
                          int64_t height) {
    if (e.kind == MwebEventKind::ADD) {
        // If this commitment was tracked as pending in the mempool, decrement
        // the pending caches first; the confirmed Add then bumps confirmed.
        // Net wallet balance is unchanged across the promote.
        PromoteMwebPendingOnConfirmLocked(c, e.commitment);
        if (e.owner_addr) {
            AddMwebUtxoLocked(c, *e.owner_addr,
                              e.commitment, e.output_id,
                              e.shared_secret,
                              e.amount_sat, height);
        }
    } else {
        // Spend confirmed: clear any pending-out marker on this commitment,
        // otherwise SpendMwebUtxo would double-deduct (pending_out reverted
        // to confirmed, then spent).
        RemovePendingMwebSpendLocked(c, e.commitment);
        SpendMwebUtxoLocked(c, e.commitment, height);
    }
}

void RollbackMwebEventLocked(OyoChainImpl& c, const MwebBlockEvent& e) {
    if (e.kind == MwebEventKind::ADD) {
        // Undo a recently-applied add: utxo was added in the rolled-back
        // block, never spent → remove entirely.
        RemoveMwebUtxoLocked(c, e.commitment);
    } else {
        UnspendMwebUtxoLocked(c, e.commitment);
    }
}

// Walks block.mweb.{inputs,outputs} and appends ALL of them to the persistent
// MWEB event-log (regardless of whether they belong to one of our keychains).
// Two passes give each event a packed monotonic id: pass 1 a
// spend event per input (BLIND — the spent output's row was recorded earlier,
// no lookup), pass 2 an output event per output. Caller wraps in the mirror's
// BEGIN/COMMIT.
void MirrorApplyMwebFromBlockLocked(
    OyoChainImpl& c, const UniValue& block, int64_t height) {
    if (!c.mweb_mirror) return;
    const UniValue& mweb = block["mweb"];
    if (!mweb.isObject()) return;

    int64_t idx = 0;   // event index within the block: spends first, then outputs

    // Pass 1 — spends.
    const UniValue& inputs = mweb["inputs"];
    if (inputs.isArray()) {
        for (size_t i = 0; i < inputs.size(); ++i) {
            const UniValue& in = inputs[i];
            if (!in.isObject()) continue;
            const std::string commit_hex = in["commit"].isStr() ? in["commit"].get_str() : std::string();
            if (commit_hex.empty()) continue;
            std::vector<uint8_t> commit_bytes = ParseHex(commit_hex);
            if (commit_bytes.size() != 33) continue;
            c.mweb_mirror->AppendSpend(MirrorEventId(height, idx++), commit_bytes);
            c.mirror_delta.mweb_spends++;
        }
    }
    // Pass 2 — outputs.
    const UniValue& outputs = mweb["outputs"];
    if (outputs.isArray()) {
        for (size_t i = 0; i < outputs.size(); ++i) {
            const UniValue& o = outputs[i];
            if (!o.isObject()) continue;
            const std::string commit_hex = o["commit"].isStr()          ? o["commit"].get_str()          : std::string();
            const std::string oid_hex    = o["output_id"].isStr()       ? o["output_id"].get_str()       : std::string();
            const std::string ko_hex     = o["receiver_pubkey"].isStr() ? o["receiver_pubkey"].get_str() : std::string();
            const std::string msg_hex    = o["message"].isStr()         ? o["message"].get_str()         : std::string();
            if (commit_hex.empty() || oid_hex.empty() || ko_hex.empty() || msg_hex.empty()) continue;

            std::vector<uint8_t> commit_bytes  = ParseHex(commit_hex);
            std::vector<uint8_t> oid_bytes     = ParseHex(oid_hex);
            std::vector<uint8_t> ko_bytes      = ParseHex(ko_hex);
            std::vector<uint8_t> msg_bytes     = ParseHex(msg_hex);
            if (commit_bytes.size() != 33 || oid_bytes.size() != 32 || ko_bytes.size() != 33) continue;

            c.mweb_mirror->AppendOutput(MirrorEventId(height, idx++), commit_bytes, oid_bytes,
                                        ko_bytes, msg_bytes);
            c.mirror_delta.mweb_outputs++;
        }
    }
}

// --- two-mode bootstrap: batched persistent-mirror transaction --------------
// All mirror writes go through one LevelDB WriteBatch per transaction. During
// the genesis walk (bootstrap_done=0) the regular + MWEB + trail writes are
// committed in large batches of kBootstrapBatchBlocks for throughput; working
// mode commits per block (target 1) for prompt visibility. Secondary keys are
// written inline (LevelDB has no deferred index build), so the only difference
// between the two modes is commit cadence. Commits are not fsync'd: the mirror
// is a node-derived cache, the LevelDB log keeps each committed batch atomic
// across a crash, and the block trail resumes from the last committed height —
// an unclean exit costs a few re-walked blocks, never a torn block or index.
void MirrorBeginBatchLocked(OyoChainImpl& c) {
    if (!c.mweb_mirror) return;
    if (!c.mirror_txn_open) {
        c.mweb_mirror->Begin();
        c.mirror_delta.Clear();
        c.mirror_txn_open = true;
    }
}
void MirrorCommitBatchLocked(OyoChainImpl& c, bool sync = false) {
    if (!c.mweb_mirror || !c.mirror_txn_open) return;
    int64_t t0 = NowMicros();
    try {
        c.mweb_mirror->ApplyDelta(c.mirror_delta);
        c.mweb_mirror->Commit(sync);
    } catch (...) {
        try { c.mweb_mirror->Rollback(); } catch (...) {}
        c.mirror_txn_open = false;
        c.mirror_batch_count = 0;
        c.mirror_delta.Clear();
        throw;
    }
    TimingAddSince(c.sync_perf.mirror_commit, t0);
    c.mirror_txn_open    = false;
    c.mirror_batch_count = 0;
    c.mirror_delta.Clear();
}
void MirrorRollbackBatchLocked(OyoChainImpl& c) {
    if (!c.mweb_mirror || !c.mirror_txn_open) return;
    try { c.mweb_mirror->Rollback(); } catch (...) {}
    c.mirror_txn_open    = false;
    c.mirror_batch_count = 0;
    c.mirror_delta.Clear();
}
// First time the walk reaches the node tip: flush the final bootstrap batch,
// stamp the working state, and flip to per-block commit cadence. LevelDB needs
// no index-build step — secondary keys were written inline during the walk.
void MirrorFinalizeBootstrapLocked(OyoChainImpl& c) {
    if (!c.mweb_mirror) return;
    MirrorCommitBatchLocked(c);
    c.mweb_mirror->MetaSet(kMetaState, kMirrorWorking);
    c.mirror_batch_target = 1;
    c.bootstrap_done      = true;
}

// Trim the reorg-only output/spend journals to the last max_reorg_depth blocks.
// Called per block inside the open mirror batch (atomic with the block's writes).
// keep_floor = tip - max_reorg_depth; deeper reorgs are rejected by the sync
// depth-guards and rebuild from genesis, so journal rows below the floor can
// never be needed. The persisted floor advances only when the region below it is
// fully clean, so the reorg guard never over-claims.
void PruneReorgLogsLocked(OyoChainImpl& c, int64_t tip_height) {
    if (!c.regular_mirror || !c.mweb_mirror) return;
    int64_t keep = tip_height - c.max_reorg_depth;
    if (keep <= 0) return;
    // Resume from the persisted watermark: everything below it is already
    // deleted, so we Seek past the tombstones straight to the live tail.
    int64_t start = c.mweb_mirror->MetaGet(kMetaRegularPruneFloor, 0);
    if (start >= keep) return;  // nothing new to trim this block
    bool caught = c.regular_mirror->PruneJournalsBelow(start, keep, kPruneMaxDeletesPerCall);
    if (caught) c.mweb_mirror->MetaSet(kMetaRegularPruneFloor, keep);
}

void ApplyBlockLocked(OyoChainImpl& c, const UniValue& block, int64_t height, const std::string& hash) {
    int64_t t_block = NowMicros();
    Block b;
    b.height = height;
    b.hash = hash;
    int64_t t0 = NowMicros();
    b.events = ParseBlockEvents(block);
    TimingAddSince(c.sync_perf.parse_regular_events, t0);
    t0 = NowMicros();
    for (const auto& e : b.events) ApplyEventLocked(c, e, height);
    TimingAddSince(c.sync_perf.apply_regular_events, t0);

    t0 = NowMicros();
    auto mweb_events = ParseMwebBlockEventsLocked(c, block);
    TimingAddSince(c.sync_perf.parse_mweb_events, t0);
    t0 = NowMicros();
    for (const auto& e : mweb_events) ApplyMwebEventLocked(c, e, height);
    TimingAddSince(c.sync_perf.apply_mweb_events, t0);

    // Persistent event-logs (regular + MWEB) + trail update: every output/spend
    // in the block is appended to the DB (even ones we don't own — needed so
    // future wallet bootstraps can RewindOutput against the historical set).
    // All persistent mirror keys share one LevelDB WriteBatch, so a committed
    // block has its events, counters, and trail atomically.
    if (c.mweb_mirror) {
        MirrorBeginBatchLocked(c);
        try {
            t0 = NowMicros();
            MirrorApplyMwebFromBlockLocked(c, block, height);
            TimingAddSince(c.sync_perf.mirror_mweb_feed, t0);
            // Regular mirror and MWEB/trail are both inside batch transactions; in
            // bootstrap mode the commit spans up to mirror_batch_target blocks.
            t0 = NowMicros();
            FeedRegularMirrorLocked(c, block, height);
            TimingAddSince(c.sync_perf.mirror_regular_feed, t0);
            c.block_trail->Put(height, HexToBytes(hash));
            PruneReorgLogsLocked(c, height);   // trim o/s journals to the reorg window (same batch)
            if (++c.mirror_batch_count >= c.mirror_batch_target)
                MirrorCommitBatchLocked(c);
        } catch (...) {
            MirrorRollbackBatchLocked(c);
            throw;
        }
    }

    c.blocks.push_back(std::move(b));
    c.mweb_events_per_block.push_back(std::move(mweb_events));
    c.tip_height = height;
    c.tip_hash = hash;

    // #4: a tx confirmed in this block has left the node mempool — drop it from
    // the pending index immediately (don't wait for the next mempool diff) so a
    // just-confirmed receive isn't briefly shown as confirmed AND pending. A
    // reorg that un-confirms it returns it to the mempool, where the next
    // mempool sync re-indexes it.
    {
        const UniValue& btxs = block["tx"];
        if (btxs.isArray()) {
            for (size_t ti = 0; ti < btxs.size(); ++ti) {
                const UniValue& tv = btxs[ti]["txid"];
                if (tv.isStr()) UnindexMempoolTxLocked(c, LowerHex(tv.get_str()));
            }
        }
    }

    while (c.blocks.size() > c.rollback_window) {
        // The front block is leaving the ring — reorg cannot reach it anymore.
        Block& front = c.blocks.front();
        for (const auto& e : front.events) {
            auto it = c.addresses.find(e.script_hex);
            if (it == c.addresses.end()) continue;
            Address& addr = *it->second;
            GcUtxoLocked(addr, e.key);
        }
        // GC MWEB utxos that this aged-out block recorded as SPEND. ADD
        // events leave their utxos in the index — they're still alive and
        // referenced by future blocks, only spends remove them.
        if (!c.mweb_events_per_block.empty()) {
            for (const auto& e : c.mweb_events_per_block.front()) {
                if (e.kind == MwebEventKind::SPEND) {
                    GcMwebUtxoLocked(c, e.commitment);
                }
            }
            c.mweb_events_per_block.pop_front();
        }
        c.blocks.pop_front();
    }
    // NOTE: the MWEB mirror is NOT pruned of spent rows — it keeps the full
    // journal (spent_height marks them) symmetrically with regular_utxo, so
    // MWEB outputs have history too. (It used to drop spent rows aged past the
    // rollback window; removed for journal symmetry — disk is the only cost.)
    int64_t elapsed = NowMicros() - t_block;
    c.sync_perf.blocks++;
    c.sync_perf.last_block_height = height;
    c.sync_perf.last_block_us = elapsed;
    TimingAdd(c.sync_perf.apply_block, elapsed);
}

bool RollbackPersistentMirrorToLocked(OyoChainImpl& c, int64_t below) {
    if (!c.mweb_mirror || !c.block_trail) return false;
    // Defensively flush any in-flight bootstrap batch so this reorg runs in its
    // own transaction. Idempotent: a no-op when no batch is open (the common
    // case — the non-empty rollback path already flushed, and the restart-time
    // path has no batch). Without it, an open batch would silently swallow the
    // delete and desync mirror_txn_open.
    MirrorCommitBatchLocked(c);
    // Prune-floor guard: the o/s journals below regular_prune_floor are gone, so
    // tail-deleting into that region would find fewer rows than really existed
    // and silently undercount the meta deltas. The sync depth-guards already
    // prevent this (window >= max_reorg_depth), but enforce it here too so any
    // future caller fails loud (-> full resync) instead of corrupting counters.
    {
        int64_t prune_floor = c.mweb_mirror->MetaGet(kMetaRegularPruneFloor, 0);
        if (prune_floor > 0 && below + 1 < prune_floor)
            throw std::runtime_error("reorg target below pruned journal floor; full resync required");
    }
    c.mweb_mirror->Begin();
    try {
        MirrorDelta undo;
        if (c.regular_mirror) {
            MirrorDelta rd = c.regular_mirror->DeleteAbove(below);
            undo.regular_outputs -= rd.regular_outputs;
            undo.regular_spends  -= rd.regular_spends;
        }
        MirrorDelta md = c.mweb_mirror->DeleteAbove(below);
        undo.mweb_outputs -= md.mweb_outputs;
        undo.mweb_spends  -= md.mweb_spends;
        c.block_trail->DeleteAbove(below);
        c.mweb_mirror->ApplyDelta(undo);
        c.mweb_mirror->Commit();
    } catch (...) {
        c.mweb_mirror->Rollback();
        throw;
    }

    int64_t t = c.block_trail->Tip();
    if (t < 0) {
        c.tip_height = -1;
        c.tip_hash.clear();
        return true;
    }
    std::vector<uint8_t> hb = c.block_trail->HashAt(t);
    if (hb.size() != 32) return false;
    c.tip_height = t;
    c.tip_hash = ToHexLower(hb.data(), 32);
    return true;
}

bool RollbackTipLocked(OyoChainImpl& c) {
    if (c.blocks.empty()) {
        int64_t t = c.block_trail ? c.block_trail->Tip() : -1;
        if (t < 0) return false;
        bool ok = RollbackPersistentMirrorToLocked(c, t - 1);
        if (ok) InvalidateLoadedWalletMirrorStateLocked(c);
        return ok;
    }
    // Persistent rollback FIRST — it is the only step here that can fail (a
    // LevelDB write/disk error) and it commits atomically (the batch is
    // discarded whole on error). Running it before any in-memory wallet mutation
    // means a failure leaves BOTH stores at the pre-reorg tip — no divergence to
    // heal. It also flushes any in-flight bootstrap batch internally. The
    // in-memory undo below only touches RAM maps (it never reads the rows just
    // deleted), so this ordering is safe.
    const int64_t reorg_height = c.blocks.back().height;
    if (!RollbackPersistentMirrorToLocked(c, reorg_height - 1)) return false;

    Block b = std::move(c.blocks.back());
    c.blocks.pop_back();
    // Reverse order to undo in reverse of apply.
    for (auto it = b.events.rbegin(); it != b.events.rend(); ++it) {
        RollbackEventLocked(c, *it);
    }
    if (!c.mweb_events_per_block.empty()) {
        std::vector<MwebBlockEvent> me = std::move(c.mweb_events_per_block.back());
        c.mweb_events_per_block.pop_back();
        for (auto it = me.rbegin(); it != me.rend(); ++it) {
            RollbackMwebEventLocked(c, *it);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Wallet / chain status JSON
// ---------------------------------------------------------------------------

UniValue ChainStatusJsonLocked(const OyoChainImpl& c) {
    UniValue root(UniValue::VOBJ);
    root.pushKV("network", c.network);
    root.pushKV("tip_height", c.tip_height);
    root.pushKV("tip_hash", c.tip_hash);
    root.pushKV("rollback_window", int64_t(c.rollback_window));
    root.pushKV("max_reorg_depth", c.max_reorg_depth);
    root.pushKV("regular_prune_floor",
                c.mweb_mirror ? c.mweb_mirror->MetaGet(kMetaRegularPruneFloor, 0) : 0);
    root.pushKV("blocks_cached", int64_t(c.blocks.size()));
    root.pushKV("addresses_tracked", int64_t(c.addresses.size()));
    root.pushKV("track_mempool", c.track_mempool);
    root.pushKV("native_block_parse", c.native_block_parse);
    root.pushKV("mempool_size", int64_t(c.mempool.size()));
    root.pushKV("mempool_seen_not_ours", int64_t(c.mempool_seen_not_ours.size()));
    root.pushKV("mweb_pending_outputs", int64_t(c.mweb_mempool_pending.size()));
    root.pushKV("mweb_pending_spends",  int64_t(c.mweb_mempool_pending_out.size()));
    root.pushKV("mweb_utxo_index_size", int64_t(c.mweb_utxo_index.size()));

    {
        const SyncPerfStats& p = c.sync_perf;
        UniValue prof(UniValue::VOBJ);
        prof.pushKV("blocks", p.blocks);
        prof.pushKV("last_block_height", p.last_block_height);
        prof.pushKV("last_block_ms", p.last_block_us / 1000);
        UniValue stages(UniValue::VOBJ);
        stages.pushKV("native_decode",        TimingJson(p.native_decode));
        stages.pushKV("apply_block",          TimingJson(p.apply_block));
        stages.pushKV("parse_regular_events", TimingJson(p.parse_regular_events));
        stages.pushKV("apply_regular_events", TimingJson(p.apply_regular_events));
        stages.pushKV("parse_mweb_events",    TimingJson(p.parse_mweb_events));
        stages.pushKV("apply_mweb_events",    TimingJson(p.apply_mweb_events));
        stages.pushKV("mirror_mweb_feed",     TimingJson(p.mirror_mweb_feed));
        stages.pushKV("mirror_regular_feed",  TimingJson(p.mirror_regular_feed));
        stages.pushKV("mirror_commit",        TimingJson(p.mirror_commit));
        prof.pushKV("stages", stages);
        root.pushKV("sync_profile", prof);
    }

    // Persistent UTXO mirrors: regular (canonical full journal) + MWEB,
    // each as total (spent + unspent) / unspent. Counts() is O(1): it reads the
    // integer meta counters maintained by per-batch deltas and rollback delete counts.
    if (c.regular_mirror && c.mweb_mirror) {
        UniValue mir(UniValue::VOBJ);
        try {
            int64_t rt, ru, mt, mu;
            c.regular_mirror->Counts(rt, ru);
            c.mweb_mirror->Counts(mt, mu);
            UniValue reg(UniValue::VOBJ);
            reg.pushKV("total",   rt);
            reg.pushKV("unspent", ru);
            UniValue mw(UniValue::VOBJ);
            mw.pushKV("total",    mt);
            mw.pushKV("unspent",  mu);
            mir.pushKV("regular",       reg);
            mir.pushKV("mweb",          mw);
            mir.pushKV("synced_height", c.tip_height);
        } catch (const std::exception& e) {
            mir.pushKV("error", std::string(e.what()));
        }
        root.pushKV("utxo_mirror", mir);
    }

    UniValue wnames(UniValue::VARR);
    for (const auto& w : c.wallets) wnames.push_back(w->name);
    root.pushKV("wallets", wnames);
    return root;
}

UniValue WalletStatusJsonLocked(const OyoWalletImpl& w) {
    const OyoChainImpl& c = *w.chain;
    UniValue root(UniValue::VOBJ);
    root.pushKV("name", w.name);
    root.pushKV("type",       w.watch_only ? "watch" : "regular");
    root.pushKV("kind",       w.address_kind);
    root.pushKV("watch_only", w.watch_only);
    root.pushKV("network", c.network);
    root.pushKV("tip_height", c.tip_height);
    root.pushKV("tip_hash", c.tip_hash);
    root.pushKV("revision", int64_t(w.revision));
    // Derivation algorithm tag — frontend uses this as the wallet's
    // type label so users see "oyo_v1" / "oyo_mweb_v1" / "mweb_v0"
    // rather than a generic kind label.
    root.pushKV("seed_type", w.seed_type);
    int active_addrs = 0;
    for (const auto& b : w.bindings) if (b) active_addrs++;
    root.pushKV("address_count", active_addrs);

    int64_t pending_in       = 0;
    int64_t pending_out      = 0;
    int64_t immature         = 0;  // confirmed peg-out / coinbase vouts inside maturity
    // Per-address immature totals — used below in the addresses[] vout so
    // each row can subtract its share from "spendable" and surface the
    // immature portion separately. Same predicate as IsSpendableUtxoLocked
    // so reported immature == coin-selection's filtered-out amount.
    std::unordered_map<int, int64_t> per_addr_immature;
    for (const auto& b : w.bindings) {
        if (!b) continue;
        pending_in  += b->addr->pending_in_sat;
        pending_out += b->addr->pending_out_sat;
        int64_t addr_immature = 0;
        for (const auto& kv : b->addr->utxos) {
            const Utxo& u = kv.second;
            if (!u.confirmed || u.spent || u.spent_pending) continue;
            if (IsImmatureUtxoLocked(u, c.tip_height)) {
                immature      += u.amount_sat;
                addr_immature += u.amount_sat;
            }
        }
        if (addr_immature > 0) per_addr_immature[b->index] = addr_immature;
    }
    // available excludes immature peg-outs and coinbases — they're confirmed
    // but not yet spendable (node would reject premature spends with
    // bad-txns-premature-spend-of-{pegout,coinbase}). Mirrors node-wallet's
    // `immature` accounting.
    int64_t available = w.balance_sat - immature;
    if (available < 0) available = 0;
    root.pushKV("confirmed_sat",      w.balance_sat);
    root.pushKV("available_sat",      available);
    root.pushKV("pending_in_sat",     pending_in);
    root.pushKV("pending_out_sat",    pending_out);
    root.pushKV("immature_sat",       immature);

    // Aggregate pending across canonical + MWEB sides for the summary
    // view's `net_pending` field — same shape node-wallet uses
    // (untrusted_pending minus outgoing). MWEB-side balances (when the
    // wallet has an mweb keychain) contribute to the same totals.
    int64_t total_pending_in  = pending_in;
    int64_t total_pending_out = pending_out;
    if (w.has_mweb) {
        for (const auto& b_ptr : w.mweb_bindings) {
            if (!b_ptr || !b_ptr->addr) continue;
            total_pending_in  += b_ptr->addr->pending_in_sat;
            total_pending_out += b_ptr->addr->pending_out_sat;
        }
    }
    const double net_pending = double(total_pending_in - total_pending_out) / double(kCoin);

    UniValue balances(UniValue::VOBJ), mine(UniValue::VOBJ);
    mine.pushKV("trusted",            double(available)       / double(kCoin));
    mine.pushKV("available",          double(available)       / double(kCoin));
    mine.pushKV("untrusted_pending",  double(total_pending_in)  / double(kCoin));
    mine.pushKV("outgoing_pending",   double(total_pending_out) / double(kCoin));
    mine.pushKV("immature",           double(immature) / double(kCoin));
    mine.pushKV("net_pending",        net_pending);
    balances.pushKV("mine", mine);
    root.pushKV("balances", balances);

    UniValue addrs(UniValue::VARR);
    UniValue utxos(UniValue::VARR);
    UniValue desync_addrs(UniValue::VARR);
    bool wallet_desync = false;
    int64_t total_utxo = 0;
    for (const auto& b : w.bindings) {
        if (!b) continue;
        const Address& a = *b->addr;
        UniValue ao(UniValue::VOBJ);
        ao.pushKV("index", b->index);
        ao.pushKV("kind", a.kind);
        ao.pushKV("address", a.address);
        ao.pushKV("script_pubkey", a.script_hex);
        ao.pushKV("confirmed_sat",   a.confirmed_sat);
        ao.pushKV("pending_in_sat",  a.pending_in_sat);
        ao.pushKV("pending_out_sat", a.pending_out_sat);
        // immature_sat = portion of confirmed_sat tied up in peg-out or
        // coinbase vouts that haven't matured yet. Spendable = confirmed
        // minus immature.
        // Frontend renders this separately so users see why their full
        // confirmed balance can't be spent.
        {
            auto it = per_addr_immature.find(b->index);
            ao.pushKV("immature_sat", it != per_addr_immature.end() ? it->second : int64_t(0));
        }
        ao.pushKV("desync", a.desync);
        addrs.push_back(ao);
        if (a.desync) {
            wallet_desync = true;
            desync_addrs.push_back(b->index);
        }
        for (const auto& kv : a.utxos) {
            const Utxo& u = kv.second;
            if (u.spent) continue;
            UniValue uo(UniValue::VOBJ);
            uo.pushKV("txid", u.txid);
            uo.pushKV("vout", int64_t(u.vout));
            uo.pushKV("amount_sat", u.amount_sat);
            uo.pushKV("height", u.height);
            uo.pushKV("confirmed", u.confirmed);
            uo.pushKV("immature", IsImmatureUtxoLocked(u, c.tip_height));
            uo.pushKV("spent_pending", u.spent_pending);
            if (u.spent_pending) uo.pushKV("spent_pending_txid", u.spent_pending_txid);
            uo.pushKV("address", a.address);
            uo.pushKV("script_pubkey", a.script_hex);
            uo.pushKV("kind", a.kind);
            uo.pushKV("index", b->index);
            utxos.push_back(uo);
            total_utxo++;
        }
    }
    root.pushKV("addresses", addrs);
    root.pushKV("utxos", utxos);
    root.pushKV("utxo_count", total_utxo);
    root.pushKV("desync", wallet_desync);
    root.pushKV("desync_addresses", desync_addrs);

    if (w.has_mweb) {
        UniValue mw_addrs(UniValue::VARR);
        UniValue mw_utxos(UniValue::VARR);
        int64_t mw_total = 0;
        // Aggregate pending across this wallet's bindings — same shape
        // canonical uses for its own pending sums (see canonical block
        // earlier in this function).
        int64_t mw_pending_in  = 0;
        int64_t mw_pending_out = 0;
        for (const auto& b_ptr : w.mweb_bindings) {
            if (!b_ptr || !b_ptr->addr) continue;
            const MwebAddress& ma = *b_ptr->addr;
            mw_pending_in  += ma.pending_in_sat;
            mw_pending_out += ma.pending_out_sat;
            UniValue mao(UniValue::VOBJ);
            mao.pushKV("index",            int64_t(ma.address_index));
            mao.pushKV("address",          ma.mweb_address);
            mao.pushKV("scan_pubkey",      HexStr(ma.scan_pubkey));
            mao.pushKV("spend_pubkey",     HexStr(ma.spend_pubkey));
            mao.pushKV("confirmed_sat",    ma.confirmed_sat);
            mao.pushKV("pending_in_sat",   ma.pending_in_sat);
            mao.pushKV("pending_out_sat",  ma.pending_out_sat);
            mao.pushKV("desync",           ma.desync);
            // Receive HISTORY (spent + unspent) for a stealth address is served
            // by oyo_chain_address_status's MWEB arm (BuildMwebAddressStatusLocked),
            // not here — the wallet status stays the live-balance view. Both read
            // the same chain-level MwebAddress.utxos.
            mw_addrs.push_back(mao);
            for (const auto& kv : ma.utxos) {
                const MwebUtxo& mu = kv.second;
                if (mu.spent) continue;
                UniValue uo(UniValue::VOBJ);
                uo.pushKV("commitment",  HexStr(mu.commitment));
                uo.pushKV("output_id",   HexStr(mu.output_id));
                uo.pushKV("amount_sat",  mu.amount_sat);
                uo.pushKV("height",      mu.height);
                uo.pushKV("confirmed",   mu.confirmed);
                uo.pushKV("address",     ma.mweb_address);
                uo.pushKV("address_idx", int64_t(ma.address_index));
                mw_utxos.push_back(uo);
                mw_total++;
            }
        }
        UniValue mw(UniValue::VOBJ);
        mw.pushKV("kind",              "mweb");
        mw.pushKV("address_count",     int64_t(w.mweb_bindings.size()));
        mw.pushKV("balance_sat",       w.mweb_balance_sat);
        mw.pushKV("pending_in_sat",    mw_pending_in);
        mw.pushKV("pending_out_sat",   mw_pending_out);
        mw.pushKV("addresses",         mw_addrs);
        mw.pushKV("utxos",             mw_utxos);
        mw.pushKV("utxo_count",        mw_total);
        root.pushKV("mweb", mw);
    }
    return root;
}

// ---------------------------------------------------------------------------
// Sync state machine
// ---------------------------------------------------------------------------

std::string MakeRpc(const std::string& id, const std::string& method, const UniValue& params) {
    UniValue req(UniValue::VOBJ);
    req.pushKV("jsonrpc", "1.0");
    req.pushKV("id", id);
    req.pushKV("method", method);
    req.pushKV("params", params);
    return req.write();
}

// Default fee rate when estimatesmartfee fails (regtest, fresh estimator,
// etc.). 1 sat/vB = network min relay rate, always sufficient for regtest
// and a sane floor for testnet/mainnet.
constexpr uint64_t kFallbackFeeRateSatPerVb = 1;

// Anti-fee-sniping locktime — mirrors node-wallet's txassembler.cpp:485-518.
// Returns chain.tip_height (assumed current) with 10% probability of being
// 0..100 blocks lower for high-latency / mixnet privacy. Returns 0 if the
// chain isn't synced yet, matching node behaviour for pre-IBD wallets.
uint32_t AntiFeeSnipingLocktime(const OyoChainImpl& chain) {
    if (chain.tip_height < 0) return 0;
    uint32_t lt = static_cast<uint32_t>(chain.tip_height);
    if (lt == 0) return 0;
    if (GetRandInt(10) == 0) {
        int back = GetRandInt(100);
        if (back > static_cast<int>(lt)) back = static_cast<int>(lt);
        lt = lt - static_cast<uint32_t>(back);
    }
    return lt;
}

// Builds the estimatesmartfee RPC envelope for an op needing a fee rate.
// Conservatives 6-block target, ECONOMICAL mode (mempool-driven only —
// matches what node's wallet uses for unconstrained sends).
void EmitEstimateSmartFeeRpc(OyoOpImpl& op) {
    UniValue params(UniValue::VARR);
    params.push_back(int64_t(6));
    params.push_back("ECONOMICAL");
    op.rpc_req = MakeRpc("oyo-fee-est", "estimatesmartfee", params);
    op.state   = OYO_OP_NEED_RPC;
}

// Parses a `estimatesmartfee` result envelope into a sat/vB integer rate.
// Returns kFallbackFeeRateSatPerVb on -1 / errors / non-finite values.
// estimatesmartfee returns LTC/kB; sat/vB = LTC/kB * 1e5.
uint64_t ResolveFeeRateFromEstimateEnv(const UniValue& env) {
    try {
        UniValue r = env["result"];
        if (!r.isObject()) return kFallbackFeeRateSatPerVb;
        const UniValue& fr = r["feerate"];
        if (!fr.isNum()) return kFallbackFeeRateSatPerVb;
        double ltc_per_kb = fr.get_real();
        if (!std::isfinite(ltc_per_kb) || ltc_per_kb <= 0) {
            return kFallbackFeeRateSatPerVb;
        }
        double sat_per_vb = ltc_per_kb * 1e5;
        if (sat_per_vb < 1.0) return 1;
        if (sat_per_vb > 100000.0) return 100000;
        return static_cast<uint64_t>(std::ceil(sat_per_vb));
    } catch (...) {
        return kFallbackFeeRateSatPerVb;
    }
}

UniValue UnwrapRpcResult(const UniValue& env) {
    if (!env.isObject()) throw std::runtime_error("rpc envelope not object");
    const UniValue& err = env["error"];
    if (!err.isNull()) {
        std::string m = "rpc error";
        if (err.isObject()) {
            const UniValue& msg = err["message"];
            if (msg.isStr()) m = "rpc error: " + msg.get_str();
        }
        throw std::runtime_error(m);
    }
    return env["result"];
}

// #5: convert a natively-deserialized CBlock into the same UniValue shape the
// apply path consumes from getblock verbosity=2 — regular txs via the node's own
// TxToUniv (so the per-tx shape is identical by construction), MWEB via the
// subset of fields ParseMwebBlockEventsLocked reads. height/hash come from the
// walk (a raw block header carries neither height nor a verbosity-2 wrapper).
// Only the fields the apply path reads are emitted (no header/size/difficulty).
UniValue CBlockToUniValueLocked(const CBlock& block, int64_t height, const std::string& hash) {
    UniValue out(UniValue::VOBJ);
    out.pushKV("hash", hash);
    out.pushKV("height", height);
    UniValue txs(UniValue::VARR);
    for (const auto& tx : block.vtx) {
        UniValue objTx(UniValue::VOBJ);
        TxToUniv(*tx, uint256(), objTx, /*include_hex=*/false, /*serialize_flags=*/0);
        txs.push_back(objTx);
    }
    out.pushKV("tx", txs);
    if (!block.mweb_block.IsNull() && block.mweb_block.m_block) {
        UniValue mweb(UniValue::VOBJ);
        UniValue inputs(UniValue::VARR);
        for (const auto& in : block.mweb_block.m_block->GetInputs()) {
            UniValue oi(UniValue::VOBJ);
            oi.pushKV("commit", in.GetCommitment().ToHex());
            oi.pushKV("output_id", in.GetOutputID().ToHex());
            inputs.push_back(oi);
        }
        mweb.pushKV("inputs", inputs);
        UniValue outputs(UniValue::VARR);
        for (const auto& o : block.mweb_block.m_block->GetOutputs()) {
            UniValue oo(UniValue::VOBJ);
            oo.pushKV("output_id", o.GetOutputID().ToHex());
            oo.pushKV("commit", o.GetCommitment().ToHex());
            oo.pushKV("receiver_pubkey", o.GetReceiverPubKey().ToHex());
            oo.pushKV("message", HexStr(o.GetOutputMessage().Serialized()));
            outputs.push_back(oo);
        }
        mweb.pushKV("outputs", outputs);
        out.pushKV("mweb", mweb);
    }
    return out;
}

void SyncEmitBlockchainInfo(OyoOpImpl& op) {
    op.rpc_req = MakeRpc("oyo-sync", "getblockchaininfo", UniValue(UniValue::VARR));
    op.phase = int32_t(SyncPhase::WaitBlockchainInfo);
    op.state = OYO_OP_NEED_RPC;
}
void SyncEmitAnchor(OyoOpImpl& op) {
    UniValue params(UniValue::VARR);
    params.push_back(op.chain->tip_height);
    op.rpc_req = MakeRpc("oyo-sync", "getblockhash", params);
    op.phase = int32_t(SyncPhase::WaitAnchorHash);
    op.state = OYO_OP_NEED_RPC;
}
void SyncEmitNextHash(OyoOpImpl& op) {
    UniValue params(UniValue::VARR);
    params.push_back(op.chain->tip_height + 1);
    op.walk_height = op.chain->tip_height + 1;
    op.rpc_req = MakeRpc("oyo-sync", "getblockhash", params);
    op.phase = int32_t(SyncPhase::WaitNextHash);
    op.state = OYO_OP_NEED_RPC;
}
void SyncEmitGetBlock(OyoOpImpl& op) {
    UniValue params(UniValue::VARR);
    params.push_back(op.walk_hash);
    // Native path: verbosity 0 (raw hex → CBlock). Else verbosity 2 (full JSON).
    params.push_back(op.chain->native_block_parse ? int64_t(0) : int64_t(2));
    op.rpc_req = MakeRpc("oyo-sync", "getblock", params);
    op.phase = int32_t(SyncPhase::WaitBlock);
    op.state = OYO_OP_NEED_RPC;
}

void FinishSyncOk(OyoOpImpl& op, const char* status) {
    OyoChainImpl& c = *op.chain;
    // Flush any open bootstrap batch so nothing stays uncommitted across the op
    // yield (and a reorg op never meets an in-flight transaction).
    MirrorCommitBatchLocked(c);
    // First time the walk reaches the node tip: switch to working mode (per-block
    // commit). LevelDB has no deferred index to build — keys are written inline.
    if (!c.bootstrap_done && std::string(status) == "synced") {
        MirrorFinalizeBootstrapLocked(c);
    }
    UniValue r(UniValue::VOBJ);
    r.pushKV("status", status);
    r.pushKV("tip_height", op.chain->tip_height);
    r.pushKV("tip_hash", op.chain->tip_hash);
    r.pushKV("blocks_applied", op.blocks_applied);
    op.result_json = r.write();
    op.phase = int32_t(SyncPhase::Done);
    op.state = OYO_OP_DONE;
}

void StartSyncOp(OyoOpImpl& op) {
    op.phase = int32_t(SyncPhase::Init);
    op.blocks_applied = 0;
    op.rollback_start_height = op.chain ? op.chain->tip_height : -1;
    SyncEmitBlockchainInfo(op);
}

void SyncBlockchainInfo(OyoOpImpl& op, const UniValue& r) {
    const UniValue& chain = r["chain"];
    const UniValue& blocks = r["blocks"];
    const UniValue& best = r["bestblockhash"];
    if (!blocks.isNum() || !best.isStr()) throw std::runtime_error("getblockchaininfo missing fields");
    if (chain.isStr() && op.chain->network.empty()) {
        op.chain->network = NormalizeNetwork(chain.get_str());
        op.chain->hrp = NetworkHrp(op.chain->network);
    }
    op.target_height = blocks.get_int64();
    op.chain->node_tip_hint = op.target_height;  // lets the mirror feed skip pre-window reorg journals
    op.target_hash = LowerHex(best.get_str());

    if (op.chain->tip_height < 0) {
        // Self-seed: walk from genesis instead of jumping to tip. tip stays -1
        // → SyncEmitNextHash emits getblockhash(0); the mirror is built from
        // blocks (no scantxoutset/listutxos). Chunked at kMaxBlocksPerSyncCall;
        // caller re-drives until "synced".
        if (op.target_height < 0) { FinishSyncOk(op, "synced"); return; }
        SyncEmitNextHash(op);
        return;
    }
    if (op.chain->tip_height == op.target_height && op.chain->tip_hash == op.target_hash) {
        FinishSyncOk(op, "synced");
        return;
    }
    if (op.chain->tip_height > op.target_height) {
        const int64_t depth = op.chain->tip_height - op.target_height;
        if (depth > op.chain->max_reorg_depth) {
            throw std::runtime_error("chain sync reorg exceeds max_reorg_depth");
        }
        while (op.chain->tip_height > op.target_height) {
            if (!RollbackTipLocked(*op.chain)) throw std::runtime_error("chain sync rollback failed");
        }
        if (op.chain->tip_height < 0) {
            op.chain->blocks.clear();
            op.chain->mweb_events_per_block.clear();
            SyncEmitNextHash(op);
            return;
        }
    }
    SyncEmitAnchor(op);
}

void SyncAnchor(OyoOpImpl& op, const UniValue& r) {
    if (!r.isStr()) throw std::runtime_error("getblockhash returned non-string");
    std::string h = LowerHex(r.get_str());
    if (h == op.chain->tip_hash) {
        if (op.chain->tip_height >= op.target_height) { FinishSyncOk(op, "synced"); return; }
        SyncEmitNextHash(op);
        return;
    }
    if (op.rollback_start_height >= 0 &&
        (op.rollback_start_height - op.chain->tip_height) >= op.chain->max_reorg_depth) {
        throw std::runtime_error("chain sync reorg exceeds max_reorg_depth");
    }
    if (!RollbackTipLocked(*op.chain)) throw std::runtime_error("chain sync rollback failed");
    if (op.chain->tip_height < 0) {
        op.chain->blocks.clear();
        op.chain->mweb_events_per_block.clear();
        SyncEmitNextHash(op);
        return;
    }
    SyncEmitAnchor(op);
}

void SyncNextHash(OyoOpImpl& op, const UniValue& r) {
    if (!r.isStr()) throw std::runtime_error("getblockhash returned non-string");
    op.walk_hash = LowerHex(r.get_str());
    SyncEmitGetBlock(op);
}

void SyncBlock(OyoOpImpl& op, const UniValue& r) {
    OyoChainImpl& c = *op.chain;
    if (c.native_block_parse) {
        // Raw block hex (getblock verbosity 0) → CBlock → the same UniValue
        // shape the apply path expects. height/hash come from the walk; verify
        // the deserialized block hashes to the one we asked for.
        if (!r.isStr()) throw std::runtime_error("getblock(0) result not a hex string");
        int64_t t0 = NowMicros();
        CBlock block;
        if (!DecodeHexBlk(block, r.get_str())) throw std::runtime_error("native block deserialize failed");
        if (block.GetHash().GetHex() != op.walk_hash) throw std::runtime_error("native block hash mismatch");
        UniValue ublock = CBlockToUniValueLocked(block, op.walk_height, op.walk_hash);
        TimingAddSince(c.sync_perf.native_decode, t0);
        ApplyBlockLocked(c, ublock, op.walk_height, op.walk_hash);
    } else {
        if (!r.isObject()) throw std::runtime_error("getblock result not object");
        const UniValue& h = r["height"];
        const UniValue& hash = r["hash"];
        if (!h.isNum() || !hash.isStr()) throw std::runtime_error("block missing height/hash");
        int64_t height = h.get_int64();
        if (height != op.walk_height) throw std::runtime_error("block height mismatch");
        ApplyBlockLocked(c, r, height, LowerHex(hash.get_str()));
    }
    op.blocks_applied++;
    if (c.tip_height >= op.target_height) { FinishSyncOk(op, "synced"); return; }
    if (op.blocks_applied >= kMaxBlocksPerSyncCall) { FinishSyncOk(op, "partial"); return; }
    SyncEmitNextHash(op);
}

void DispatchSync(OyoOpImpl& op, const UniValue& env) {
    UniValue r = UnwrapRpcResult(env);
    switch (SyncPhase(op.phase)) {
        case SyncPhase::WaitBlockchainInfo: SyncBlockchainInfo(op, r); break;
        case SyncPhase::WaitAnchorHash:     SyncAnchor(op, r); break;
        case SyncPhase::WaitNextHash:       SyncNextHash(op, r); break;
        case SyncPhase::WaitBlock:          SyncBlock(op, r); break;
        default: throw std::runtime_error("sync response in unexpected phase");
    }
}

// Mirror-based rescan (engine == Mirror): rebuilds the in-scope addresses'
// confirmed-unspent set from the persistent regular mirror instead of a node
// scantxoutset round-trip. Same reconciliation as DispatchRescan (drop current
// confirmed-unspent, keep pending + spent, re-add from source, clear desync)
// but the source is local — zero node UTXO queries. is_pegout comes straight
// from the persistent mirror flag (kept forever). Synchronous: op goes straight
// to DONE. Doubles as
// the restart-bootstrap path: the mirror is persistent, so a fresh process
// repopulates addr.utxos from disk and adopts the persisted tip.
void RunWalletRescanFromMirrorLocked(OyoOpImpl& op) {
    OyoWalletImpl& w = *op.wallet;
    OyoChainImpl&  c = *op.chain;
    if (!c.regular_mirror) throw std::runtime_error("regular mirror not open");

    std::unordered_map<std::string, Address*> by_script;
    if (op.rescan_scripts.empty()) {
        for (const auto& b : w.bindings) {
            if (!b) continue;
            by_script[b->addr->script_hex] = b->addr;
        }
    } else {
        for (const auto& s : op.rescan_scripts) {
            for (const auto& b : w.bindings) {
                if (b && b->addr->script_hex == s) { by_script[s] = b->addr; break; }
            }
        }
    }

    for (auto& kv : by_script) {
        Address& a = *kv.second;
        std::vector<UtxoKey> to_remove;
        int64_t delta = 0;
        for (auto& ukv : a.utxos) {
            const Utxo& u = ukv.second;
            if (u.confirmed && !u.spent && !u.spent_pending) {
                delta -= u.amount_sat;
                to_remove.push_back(ukv.first);
            }
        }
        for (const auto& k : to_remove) a.utxos.erase(k);
        a.confirmed_sat += delta;
        NotifyWatchersLocked(a, delta);

        std::vector<uint8_t> script = HexToBytes(a.script_hex);
        c.regular_mirror->ForEachUnspentByScript(script,
            [&](const std::vector<uint8_t>& outpoint, int64_t amount,
                int64_t height, int64_t flags) {
                if (outpoint.size() != 36) return;
                UtxoKey k{ToHexLower(outpoint.data(), 32),
                          uint32_t(outpoint[32]) | (uint32_t(outpoint[33]) << 8) |
                          (uint32_t(outpoint[34]) << 16) | (uint32_t(outpoint[35]) << 24)};
                bool is_pegout   = (flags & RegularMirror::kFlagPegOut)   != 0;
                bool is_coinbase = (flags & RegularMirror::kFlagCoinbase) != 0;
                // Idempotent on an existing key (pending entry) — matches the
                // scantxoutset path's AddUtxoLocked dup-key short-circuit.
                AddUtxoLocked(a, k, amount, height, /*confirmed=*/true,
                              is_pegout, is_coinbase);
            });
        SetAddressDesyncLocked(a, false);
    }

    // Cold start: adopt the mirror's persisted tip so forward sync resumes from
    // disk (restart bootstrap — no node UTXO query needed).
    if (c.tip_height < 0 && c.block_trail) {
        int64_t t = c.block_trail->Tip();
        if (t >= 0) {
            std::vector<uint8_t> hb = c.block_trail->HashAt(t);
            if (hb.size() == 32) {
                c.tip_height = t;
                c.tip_hash   = ToHexLower(hb.data(), 32);
            }
        }
    }

    UniValue rr(UniValue::VOBJ);
    rr.pushKV("status", "rescanned");
    rr.pushKV("wallet", w.name);
    rr.pushKV("height", c.tip_height);
    rr.pushKV("hash", c.tip_hash);
    rr.pushKV("source", "mirror");
    UniValue scanned(UniValue::VARR);
    for (auto& kv : by_script) scanned.push_back(kv.first);
    rr.pushKV("scanned_scripts", scanned);
    int64_t total = 0;
    for (const auto& b : w.bindings) {
        if (!b) continue;
        for (const auto& kv : b->addr->utxos) if (!kv.second.spent) total++;
    }
    rr.pushKV("utxos", total);
    op.result_json = rr.write();
    op.phase = int32_t(RescanPhase::Done);
    op.state = OYO_OP_DONE;
}

// ---------------------------------------------------------------------------
// Mempool sync state machine
// ---------------------------------------------------------------------------

// Decode a mempool tx's MWEB extension from its serialized hex (the `hex` field
// of getrawtransaction verbose, which carries the MWEB part under
// rpcserialversion=2) into the per-txid cache shape. Mirrors the block-side
// extraction in CBlockToUniValueLocked, over tx.mweb_tx instead of a block's
// extension block. Returns true if the tx contributed any MWEB output or input.
bool ExtractMwebMempoolTx(const std::string& tx_hex,
                          OyoChainImpl::MwebMempoolTx* out) {
    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, tx_hex)) return false;
    if (mtx.mweb_tx.IsNull() || !mtx.mweb_tx.m_transaction) return false;
    const auto& mtxn = *mtx.mweb_tx.m_transaction;

    // Commitment -> raw 33 bytes, via the hex boundary (same idiom the rest of
    // this file uses; runs once per tx, not per poll).
    auto to_commit = [](const auto& commitment, std::array<uint8_t, 33>* arr) -> bool {
        auto v = ParseHex(commitment.ToHex());
        if (v.size() != 33) return false;
        std::memcpy(arr->data(), v.data(), 33);
        return true;
    };

    for (const auto& o : mtxn.GetOutputs()) {
        std::array<uint8_t, 33> commit_b;
        if (!to_commit(o.GetCommitment(), &commit_b)) continue;
        // Serialize the whole mw::Output exactly as oyo-mweb-mempool used to,
        // so TryRewindOutputBlob deserializes it the same way.
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << o;
        out->outputs.emplace_back(commit_b,
                                  std::vector<uint8_t>(ss.begin(), ss.end()));
    }
    for (const auto& in : mtxn.GetInputs()) {
        std::array<uint8_t, 33> commit_b;
        if (!to_commit(in.GetCommitment(), &commit_b)) continue;
        out->spends.push_back(commit_b);
    }
    return !out->outputs.empty() || !out->spends.empty();
}

// Rebuild the MWEB pending snapshot from the per-txid cache and diff it against
// the cached pending sets — the native replacement for the oyo-mweb-mempool
// diff. ADD: RewindOutput each new output per keychain, AddPendingMwebUtxo on
// match; AddPendingMwebSpend for confirmed UTXOs a mempool tx is spending.
// REMOVE: drop pending entries no longer backed by any mempool tx. Returns a
// counts object for the sync result JSON.
UniValue ReconcileMwebMempoolFromCacheLocked(OyoChainImpl& c) {
    using CommitArr = std::array<uint8_t, 33>;
    int64_t added = 0, removed = 0, sp_added = 0, sp_removed = 0, skipped = 0;

    // Skip the diff entirely when there are no MWEB consumers and nothing is
    // tracked — but still emit zeroed counts.
    if (!(c.mweb_keychains.empty() && c.mweb_utxo_index.empty() &&
          c.mweb_mempool_pending.empty() && c.mweb_mempool_pending_out.empty())) {
        std::unordered_set<CommitArr, ArrayHash33> snapshot_in, snapshot_out;

        // ---- ADD pass over the whole cache (every current mempool tx) ----
        for (const auto& kv : c.mweb_mempool_tx_cache) {
            const OyoChainImpl::MwebMempoolTx& e = kv.second;
            for (const auto& oc : e.outputs) {
                const CommitArr& commit_b = oc.first;
                snapshot_in.insert(commit_b);
                if (c.mweb_mempool_pending.count(commit_b)) continue;
                if (c.mweb_mempool_seen_not_ours.count(commit_b)) { skipped++; continue; }
                bool matched = false;
                for (const auto& kc_ptr : c.mweb_keychains) {
                    MwebKeychain& kc = *kc_ptr;
                    if (kc.spend_pubkey_index.empty()) continue;
                    CommitArr out_commit;
                    std::array<uint8_t, 32> out_oid;
                    auto rr = oyoltc::mweb::TryRewindOutputBlob(
                        kc.scan_secret, kc.spend_pubkey_index,
                        oc.second, &out_commit, &out_oid);
                    if (!rr.matched || !rr.matched_address) continue;
                    AddPendingMwebUtxoLocked(c, *rr.matched_address,
                                             commit_b, out_oid,
                                             rr.shared_secret,
                                             static_cast<int64_t>(rr.amount));
                    added++;
                    matched = true;
                    break;
                }
                if (!matched) c.mweb_mempool_seen_not_ours.insert(commit_b);
            }
            for (const auto& sc : e.spends) {
                snapshot_out.insert(sc);
                if (!c.mweb_utxo_index.count(sc)) continue;       // not ours
                if (c.mweb_mempool_pending_out.count(sc)) continue;
                AddPendingMwebSpendLocked(c, sc);
                sp_added++;
            }
        }

        // ---- REMOVE pass: pending entries no longer in the snapshot ----
        std::vector<CommitArr> drop;
        for (const auto& kv : c.mweb_mempool_pending)
            if (!snapshot_in.count(kv.first)) drop.push_back(kv.first);
        for (const auto& k : drop) { RemovePendingMwebUtxoLocked(c, k); removed++; }

        std::vector<CommitArr> seen_drop;
        for (const auto& k : c.mweb_mempool_seen_not_ours)
            if (!snapshot_in.count(k)) seen_drop.push_back(k);
        for (const auto& k : seen_drop) c.mweb_mempool_seen_not_ours.erase(k);

        std::vector<CommitArr> sp_drop;
        for (const auto& k : c.mweb_mempool_pending_out)
            if (!snapshot_out.count(k)) sp_drop.push_back(k);
        for (const auto& k : sp_drop) { RemovePendingMwebSpendLocked(c, k); sp_removed++; }
    }

    UniValue mweb(UniValue::VOBJ);
    mweb.pushKV("pending_added",    added);
    mweb.pushKV("pending_removed",  removed);
    mweb.pushKV("spends_added",     sp_added);
    mweb.pushKV("spends_removed",   sp_removed);
    mweb.pushKV("skipped_not_ours", skipped);
    return mweb;
}

void FinishMempoolOk(OyoOpImpl& op, const char* status) {
    OyoChainImpl& c = *op.chain;
    UniValue r(UniValue::VOBJ);
    r.pushKV("status", status);
    r.pushKV("mempool_size",  int64_t(c.mempool.size()));
    r.pushKV("added",         op.mempool_added_count);
    r.pushKV("rolled_back",   op.mempool_rolled_back);
    r.pushKV("skipped",       op.mempool_skipped);
    // Reconcile MWEB pending from the per-txid cache on every completed pass —
    // the native replacement for the old oyo-mweb-mempool diff op. Counts ride
    // under "mweb" so callers reading the top-level legacy fields are unaffected.
    if (std::string(status) == "synced") {
        r.pushKV("mweb", ReconcileMwebMempoolFromCacheLocked(c));
    }
    op.result_json = r.write();
    op.phase = int32_t(MempoolPhase::Done);
    op.state = OYO_OP_DONE;
}

void StartMempoolSyncOp(OyoOpImpl& op) {
    op.mempool_added.clear();
    op.mempool_added_idx = 0;
    op.mempool_rolled_back = 0;
    op.mempool_added_count = 0;
    op.mempool_skipped = 0;
    if (!op.chain->track_mempool) {
        FinishMempoolOk(op, "disabled");
        return;
    }
    op.rpc_req = MakeRpc("oyo-mempool", "getrawmempool", UniValue(UniValue::VARR));
    op.phase = int32_t(MempoolPhase::WaitMempoolList);
    op.state = OYO_OP_NEED_RPC;
}

void MempoolEmitNextTxFetch(OyoOpImpl& op) {
    while (op.mempool_added_idx < op.mempool_added.size()) {
        const std::string& txid = op.mempool_added[op.mempool_added_idx];
        // Already in chain.mempool? skip — idempotent.
        if (op.chain->mempool.count(txid) || op.chain->mempool_seen_not_ours.count(txid)) {
            op.mempool_skipped++;
            op.mempool_added_idx++;
            continue;
        }
        UniValue params(UniValue::VARR);
        params.push_back(txid);
        params.push_back(true); // verbose
        op.rpc_req = MakeRpc("oyo-mempool", "getrawtransaction", params);
        op.phase = int32_t(MempoolPhase::WaitTxData);
        op.state = OYO_OP_NEED_RPC;
        return;
    }
    FinishMempoolOk(op, "synced");
}

void MempoolHandleListResponse(OyoOpImpl& op, const UniValue& result) {
    if (!result.isArray()) throw std::runtime_error("getrawmempool result not array");
    std::unordered_map<std::string, bool> snapshot;
    snapshot.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i) {
        if (!result[i].isStr()) continue;
        snapshot.emplace(LowerHex(result[i].get_str()), true);
    }
    OyoChainImpl& c = *op.chain;
    // Removed: in our mempool but not in node snapshot.
    std::vector<std::string> removed;
    removed.reserve(c.mempool.size());
    for (const auto& kv : c.mempool) {
        if (!snapshot.count(kv.first)) removed.push_back(kv.first);
    }
    for (const auto& tid : removed) {
        RollbackPendingTxLocked(c, tid);
        op.mempool_rolled_back++;
    }
    // #4: drop pending-index entries for any tx that left the mempool — covers
    // both watched and non-watched, independent of the c.mempool / seen sets.
    {
        std::vector<std::string> gone;
        for (const auto& kv : c.mempool_view)
            if (!snapshot.count(kv.first)) gone.push_back(kv.first);
        for (const auto& tid : gone) UnindexMempoolTxLocked(c, tid);
    }
    // Drop seen-not-ours entries that left the node mempool too.
    for (auto it = c.mempool_seen_not_ours.begin(); it != c.mempool_seen_not_ours.end();) {
        if (!snapshot.count(it->first)) it = c.mempool_seen_not_ours.erase(it);
        else ++it;
    }
    // Evict MWEB tx-cache entries for txs that left the node mempool, so the
    // snapshot rebuilt in FinishMempoolOk drops their pending outputs/spends.
    for (auto it = c.mweb_mempool_tx_cache.begin(); it != c.mweb_mempool_tx_cache.end();) {
        if (!snapshot.count(it->first)) it = c.mweb_mempool_tx_cache.erase(it);
        else ++it;
    }
    // Added: in node snapshot but not in our mempool and not in seen-not-ours.
    op.mempool_added.reserve(snapshot.size());
    for (const auto& kv : snapshot) {
        if (c.mempool.count(kv.first)) continue;
        if (c.mempool_seen_not_ours.count(kv.first)) continue;
        op.mempool_added.push_back(kv.first);
    }
    MempoolEmitNextTxFetch(op);
}

void MempoolHandleTxResponse(OyoOpImpl& op, const UniValue& result) {
    if (op.mempool_added_idx >= op.mempool_added.size()) {
        FinishMempoolOk(op, "synced");
        return;
    }
    const std::string& txid = op.mempool_added[op.mempool_added_idx];
    op.mempool_added_idx++;
    OyoChainImpl& c = *op.chain;
    bool any_ours = false;
    PendingTx tx;
    tx.txid = txid;
    tx.first_seen = 0;
    if (result.isObject()) {
        IndexMempoolTxLocked(c, result, txid);   // #4: full-mempool pending index (all txs)
        ParseTxEvents(result, tx.events);
        for (const auto& e : tx.events) {
            if (c.addresses.count(e.script_hex)) { any_ours = true; break; }
        }
        // MWEB side: cache this tx's MWEB contributions, decoded from the
        // serialized hex the verbose result already carries under
        // rpcserialversion=2 (no extra RPC). The pending diff runs once per
        // pass in FinishMempoolOk. Replaces the node-side oyo-mweb-mempool poll.
        // Only pay the decode when there's an MWEB consumer to match against.
        if (!(c.mweb_keychains.empty() && c.mweb_utxo_index.empty())) {
            const UniValue& hexv = result["hex"];
            if (hexv.isStr()) {
                OyoChainImpl::MwebMempoolTx mtx_entry;
                if (ExtractMwebMempoolTx(hexv.get_str(), &mtx_entry))
                    c.mweb_mempool_tx_cache[txid] = std::move(mtx_entry);
            }
        }
    }
    if (any_ours) {
        ApplyPendingTxLocked(c, std::move(tx));
        op.mempool_added_count++;
    } else {
        c.mempool_seen_not_ours[txid] = 0;
        op.mempool_skipped++;
    }
    MempoolEmitNextTxFetch(op);
}

void DispatchMempool(OyoOpImpl& op, const UniValue& env) {
    UniValue r = UnwrapRpcResult(env);
    switch (MempoolPhase(op.phase)) {
        case MempoolPhase::WaitMempoolList: MempoolHandleListResponse(op, r); break;
        case MempoolPhase::WaitTxData:      MempoolHandleTxResponse(op, r);   break;
        default: throw std::runtime_error("mempool response in unexpected phase");
    }
}

// Per-wallet bootstrap, post-Slice 6: walks the local mirror, RewindOutput
// against this wallet's keychain, AddMwebUtxoLocked on match. No RPC. Op
// goes to Done immediately (just wraps in the standard op-pump for ABI
// continuity). Refuses if chain isn't seeded yet.
void RunWalletBootstrapLocalLocked(OyoOpImpl& op) {
    op.phase = int32_t(MwebBootstrapPhase::Init);
    OyoChainImpl& c = *op.chain;
    OyoWalletImpl& w = *op.wallet;
    if (!w.mweb_keychain) throw std::runtime_error("wallet has no mweb_keychain");
    if (!c.mweb_mirror) throw std::runtime_error("chain has no mirror");

    MwebKeychain& kc = *w.mweb_keychain;
    int64_t visited = 0;
    int64_t matched = 0;

    // Walk the WHOLE journal (spent + unspent), not just the live set: a stealth
    // address has no on-chain index, so the owner's historical (already-spent)
    // outputs can only be recovered by RewindOutput across every output. Live
    // outputs go through the normal AddMwebUtxoLocked path (balance + spend index);
    // already-spent ones are recorded history-only so the explorer can show them.
    c.mweb_mirror->ForEachAll(
        [&](const std::vector<uint8_t>& commit_v,
            const std::vector<uint8_t>& oid_v,
            const std::vector<uint8_t>& ko_v,
            const std::vector<uint8_t>& msg_b,
            int64_t  height,
            int64_t  spent_height) {
            visited++;
            if (commit_v.size() != 33 || oid_v.size() != 32 || ko_v.size() != 33) return;
            std::array<uint8_t, 33> commit_b;
            std::array<uint8_t, 32> oid_b;
            std::array<uint8_t, 33> ko_b;
            std::memcpy(commit_b.data(), commit_v.data(), 33);
            std::memcpy(oid_b.data(),    oid_v.data(),    32);
            std::memcpy(ko_b.data(),     ko_v.data(),     33);
            auto rr = oyoltc::mweb::TryRewindOutput(
                kc.scan_secret, kc.spend_pubkey_index,
                commit_b, ko_b, msg_b);
            if (!rr.matched || !rr.matched_address) return;
            if (spent_height > 0) {
                AddHistoricalSpentMwebUtxoLocked(
                    *rr.matched_address, commit_b, oid_b, rr.shared_secret,
                    static_cast<int64_t>(rr.amount), height, spent_height);
            } else {
                AddMwebUtxoLocked(c, *rr.matched_address,
                                  commit_b, oid_b,
                                  rr.shared_secret,
                                  static_cast<int64_t>(rr.amount),
                                  height);
            }
            matched++;
        },
        /*from_height=*/w.birth_height);

    UniValue r(UniValue::VOBJ);
    r.pushKV("status",       "bootstrapped");
    r.pushKV("visited",      visited);
    r.pushKV("our_outputs",  matched);
    r.pushKV("birth_height", w.birth_height);
    r.pushKV("balance_sat",  w.mweb_balance_sat);
    // tip_height/tip_hash from the chain — same shape as the old paginated
    // RPC bootstrap reported. Tests rely on these fields.
    r.pushKV("tip_height",   c.tip_height);
    r.pushKV("tip_hash",     c.tip_hash);
    op.result_json = r.write();
    op.phase = int32_t(MwebBootstrapPhase::Done);
    op.state = OYO_OP_DONE;
}


// ---------------------------------------------------------------------------
// MWEB send state machine — pure MWEB→MWEB single-recipient transfer.
// Inputs are selected largest-first from confirmed UTXOs across all
// addresses. Change is sent back to wallet's index 0 (libmw CHANGE_INDEX
// convention). Build is local; only one RPC roundtrip is needed for the
// final sendrawtransaction broadcast.
// ---------------------------------------------------------------------------

struct MwebInputCandidate {
    std::array<uint8_t, 33> commitment;
    int64_t                 amount_sat = 0;
    uint32_t                address_index = 0;
};

constexpr uint64_t kMwebSendBudgetSat = 100000;  // generous ceiling; helper rounds down

void FinalizeMwebSendOp(OyoOpImpl& op);

void StartMwebSendOp(OyoOpImpl& op) {
    op.phase = int32_t(MwebSendPhase::Init);
    OyoWalletImpl& w = *op.wallet;
    if (!w.has_mweb || !w.mweb_keychain) {
        throw std::runtime_error("wallet has no mweb side");
    }
    if (op.mweb_send_to.empty()) {
        throw std::runtime_error("missing recipient address");
    }
    if (!op.send_all && op.mweb_send_amount <= 0) {
        throw std::runtime_error("amount_sat must be > 0 (or set send_all=true)");
    }
    if (w.mweb_bindings.empty() || !w.mweb_bindings[0] || !w.mweb_bindings[0]->addr) {
        throw std::runtime_error("wallet has no index-0 change address");
    }

    if (op.fee_rate_sat_per_vb > 0) {
        FinalizeMwebSendOp(op);
        return;
    }
    EmitEstimateSmartFeeRpc(op);
    op.phase = int32_t(MwebSendPhase::WaitFeeRate);
}

void FinalizeMwebSendOp(OyoOpImpl& op) {
    OyoWalletImpl& w = *op.wallet;
    const uint64_t rate = op.fee_rate_sat_per_vb > 0
        ? op.fee_rate_sat_per_vb
        : kFallbackFeeRateSatPerVb;

    // Recipient list — single legacy entry (op.mweb_send_to + amount) or
    // the multi-recipient cfg.outputs[]. Class is uniform (validated at
    // parse time) so the path label is taken off the first entry.
    std::vector<oyoltc::mweb::SendRecipient> recipients;
    int64_t recipients_total = 0;
    if (op.send_outputs.empty()) {
        recipients.push_back({op.mweb_send_to,
                              static_cast<uint64_t>(op.mweb_send_amount)});
        recipients_total = op.mweb_send_amount;
    } else {
        recipients.reserve(op.send_outputs.size());
        for (const auto& o : op.send_outputs) {
            recipients.push_back({o.address, static_cast<uint64_t>(o.amount_sat)});
            recipients_total += o.amount_sat;
        }
    }

    // Largest-first selection across all confirmed unspent UTXOs.
    std::vector<MwebInputCandidate> all;
    for (const auto& b_ptr : w.mweb_bindings) {
        if (!b_ptr || !b_ptr->addr) continue;
        const MwebAddress& ma = *b_ptr->addr;
        for (const auto& kv : ma.utxos) {
            const MwebUtxo& u = kv.second;
            if (!u.confirmed || u.spent) continue;
            all.push_back({u.commitment, u.amount_sat, ma.address_index});
        }
    }
    std::sort(all.begin(), all.end(),
              [](const MwebInputCandidate& a, const MwebInputCandidate& b) {
                  return a.amount_sat > b.amount_sat;
              });

    // Manual input selection — replace `all` with only the caller's
    // commitments (validated against the wallet's spendable set). Caller
    // passes commitments as hex strings (op.send_inputs[].commitment is
    // hex from the JSON op config); convert to bytes for the lookup.
    if (!op.send_inputs.empty()) {
        std::unordered_map<std::array<uint8_t, 33>, const MwebInputCandidate*, ArrayHash33> by_commit;
        by_commit.reserve(all.size());
        for (const auto& cand : all) by_commit[cand.commitment] = &cand;
        std::vector<MwebInputCandidate> manual;
        manual.reserve(op.send_inputs.size());
        for (const auto& s : op.send_inputs) {
            std::array<uint8_t, 33> sc;
            auto sv = ParseHex(s.commitment);
            if (sv.size() != 33) {
                throw std::runtime_error(
                    "input commitment must be 66-hex: " + s.commitment);
            }
            std::memcpy(sc.data(), sv.data(), 33);
            auto it = by_commit.find(sc);
            if (it == by_commit.end()) {
                throw std::runtime_error(
                    "input not in wallet spendable MWEB UTXO set: " + s.commitment);
            }
            manual.push_back(*it->second);
        }
        std::sort(manual.begin(), manual.end(),
                  [](const MwebInputCandidate& x, const MwebInputCandidate& y) {
                      return x.amount_sat > y.amount_sat;
                  });
        all = std::move(manual);
    }

    std::vector<MwebInputCandidate> picked;
    int64_t total = 0;

    if (op.send_all) {
        if (all.empty()) throw std::runtime_error("no confirmed UTXOs to spend");
        picked = all;
        for (const auto& c : picked) total += c.amount_sat;
    } else {
        const int64_t need = recipients_total + int64_t(kMwebSendBudgetSat);
        for (const auto& c : all) {
            if (total >= need) break;
            picked.push_back(c);
            total += c.amount_sat;
        }
        // Manual inputs: consume every specified commitment, even if
        // the targeted-send loop already covered `need`. Caller's
        // intent — they explicitly picked these inputs.
        if (!op.send_inputs.empty() && picked.size() < all.size()) {
            for (size_t i = picked.size(); i < all.size(); ++i) {
                picked.push_back(all[i]);
                total += all[i].amount_sat;
            }
        }
        if (total < need) {
            throw std::runtime_error("insufficient funds: need=" + std::to_string(need) +
                                      " available=" + std::to_string(total));
        }
    }
    op.mweb_send_inputs_total = total;
    op.mweb_send_inputs_commit.clear();
    op.mweb_send_inputs_commit.reserve(picked.size());

    // Construct CoinSpendInfo list for the libmw helper.
    std::vector<oyoltc::mweb::CoinSpendInfo> spend_coins;
    spend_coins.reserve(picked.size());
    for (const auto& c : picked) {
        op.mweb_send_inputs_commit.push_back(c.commitment);
        const MwebAddress* a = nullptr;
        if (c.address_index < w.mweb_bindings.size() &&
            w.mweb_bindings[c.address_index]) {
            a = w.mweb_bindings[c.address_index]->addr;
        }
        if (!a) throw std::runtime_error("address slot vanished");
        auto uit = a->utxos.find(c.commitment);
        if (uit == a->utxos.end()) throw std::runtime_error("utxo vanished");
        const MwebUtxo& u = uit->second;
        oyoltc::mweb::CoinSpendInfo info;
        info.address_index = c.address_index;
        info.amount_sat    = static_cast<uint64_t>(u.amount_sat);
        info.shared_secret = u.shared_secret;
        info.output_id     = u.output_id;
        spend_coins.push_back(std::move(info));
    }

    const std::string change_address = w.mweb_bindings[0]->addr->mweb_address;

    std::array<uint8_t, 32> scan_bytes{}, spend_bytes{};
    if (w.seed_type == "oyo_mweb_v1" || w.seed_type == "oyo_v1") {
        // seed_bytes is the user's free-form string; rehash with the
        // MWEB-domain tag to get the master, same path mweb-only and
        // universal share.
        auto master = MwebMasterFromSeedString(w.seed_bytes);
        SecretKey sc = oyoltc::mweb::DeriveScanSecret(master);
        SecretKey sp = oyoltc::mweb::DeriveSpendSecret(master);
        std::memcpy(scan_bytes.data(),  sc.data(), 32);
        std::memcpy(spend_bytes.data(), sp.data(), 32);
    } else if (w.seed_type == "mweb_v0") {
        if (w.seed_bytes.size() != 64) throw std::runtime_error("malformed mweb_v0 seed");
        std::memcpy(scan_bytes.data(),  w.seed_bytes.data(),      32);
        std::memcpy(spend_bytes.data(), w.seed_bytes.data() + 32, 32);
    } else {
        throw std::runtime_error("unsupported seed_type for send");
    }

    const uint32_t locktime = AntiFeeSnipingLocktime(*op.chain);

    if (op.send_all) {
        // send_all is single-recipient only (rejected at parse time when
        // outputs[] is present), so recipients[0] holds the only target.
        // Two-pass: build with placeholder recipient (= total - budget) to
        // learn canonical_fee, then rebuild with recipient = total - fee
        // and no change. Recipient amount auto-fits.
        int64_t placeholder_recv = total - int64_t(kMwebSendBudgetSat);
        if (placeholder_recv <= 0) {
            throw std::runtime_error("insufficient funds for fee");
        }
        std::vector<oyoltc::mweb::SendRecipient> recv1{
            {recipients[0].mweb_address, uint64_t(placeholder_recv)}};
        auto built1 = oyoltc::mweb::BuildMwebSendTx(
            scan_bytes, spend_bytes, spend_coins, recv1,
            change_address, /*change=*/0, kMwebSendBudgetSat, rate, locktime);
        const uint64_t canonical_fee = built1.actual_fee_sat;
        if (int64_t(canonical_fee) >= total) {
            throw std::runtime_error("fee exceeds inputs in send_all");
        }
        const uint64_t recipient_amount = total - canonical_fee;
        std::vector<oyoltc::mweb::SendRecipient> recv2{
            {recipients[0].mweb_address, recipient_amount}};
        auto built2 = oyoltc::mweb::BuildMwebSendTx(
            scan_bytes, spend_bytes, spend_coins, recv2,
            change_address, /*change=*/0, canonical_fee, rate, locktime);
        op.mweb_send_tx_hex = built2.tx_hex;
        op.mweb_send_amount = static_cast<int64_t>(recipient_amount);
        op.mweb_send_fee    = static_cast<int64_t>(built2.actual_fee_sat);
        op.mweb_send_change = 0;
    } else {
        // Standard path: caller-specified recipient amount(s), own-MWEB
        // change recipient. Multi-recipient just passes the full
        // `recipients` vector through; the helper sums internally.
        const int64_t change_amount = total - recipients_total - int64_t(kMwebSendBudgetSat);
        if (change_amount < 0) {
            throw std::runtime_error("internal: mweb-send change underflow");
        }
        auto built = oyoltc::mweb::BuildMwebSendTx(
            scan_bytes, spend_bytes, spend_coins, recipients,
            change_address, static_cast<uint64_t>(change_amount),
            kMwebSendBudgetSat, rate, locktime);
        op.mweb_send_tx_hex = built.tx_hex;
        // Helper auto-corrects fee to canonical = weight*100 + canonical_vsize*rate
        // and folds the leftover into change.
        op.mweb_send_fee    = static_cast<int64_t>(built.actual_fee_sat);
        op.mweb_send_change = static_cast<int64_t>(built.change_sat);
        // amount_sat in the result_json (and op state) is the sum of all
        // recipient amounts — same convention regular_send uses.
        op.mweb_send_amount = recipients_total;
    }

    // Compute tx_id from the broadcast hex so the confirm cache key
    // matches what the node will return on broadcast. mweb_send_tx_hex
    // is already populated above (both send_all and standard branches).
    std::string tx_id;
    {
        std::vector<uint8_t> raw = ParseHex(op.mweb_send_tx_hex);
        if (!raw.empty()) {
            CDataStream ss(raw, SER_NETWORK, PROTOCOL_VERSION);
            CMutableTransaction parsed_mtx;
            try { ss >> parsed_mtx; tx_id = parsed_mtx.GetHash().GetHex(); } catch (...) {}
        }
    }

    if (op.dry_run) {
        // Detect path: stealth recipient → pure MWEB (M→M); canonical
        // (bech32) recipient → peg-out (M→bech32). All recipients share
        // a class (validated at parse time), so probing recipients[0]
        // is sufficient.
        const bool to_stealth = [&]() {
            try {
                CTxDestination d = DecodeDestination(recipients[0].mweb_address);
                return IsValidDestination(d) && boost::get<StealthAddress>(&d) != nullptr;
            } catch (...) { return false; }
        }();
        const std::string path_label = to_stealth ? "mweb" : "peg-out";

        // Inputs: walk the picked commitments and pull amount + owning
        // MWEB address from the wallet's bindings.
        UniValue inputs_arr(UniValue::VARR);
        for (const auto& commit : op.mweb_send_inputs_commit) {
            for (const auto& b_ptr : w.mweb_bindings) {
                if (!b_ptr || !b_ptr->addr) continue;
                const MwebAddress& a = *b_ptr->addr;
                auto uit = a.utxos.find(commit);
                if (uit == a.utxos.end()) continue;
                UniValue ent(UniValue::VOBJ);
                ent.pushKV("kind", "mweb");
                ent.pushKV("commitment", HexStr(commit));
                ent.pushKV("amount_sat", uit->second.amount_sat);
                ent.pushKV("address", a.mweb_address);
                ent.pushKV("address_index", int64_t(a.address_index));
                ent.pushKV("status", uit->second.confirmed ? "confirmed" : "pending");
                inputs_arr.push_back(ent);
                break;
            }
        }

        // Outputs: enumerate every recipient followed by the own-MWEB
        // change vout (when present). Path-correct kind ("mweb" vs
        // "pegout") so the modal renders the right scheme per row.
        UniValue outputs_arr(UniValue::VARR);
        for (const auto& r : recipients) {
            UniValue recip(UniValue::VOBJ);
            recip.pushKV("kind", to_stealth ? "mweb" : "pegout");
            recip.pushKV("address", r.mweb_address);
            recip.pushKV("amount_sat", int64_t(r.amount_sat));
            recip.pushKV("label", "recipient");
            outputs_arr.push_back(recip);
        }
        if (op.mweb_send_change > 0 && !w.mweb_bindings.empty() &&
            w.mweb_bindings[0] && w.mweb_bindings[0]->addr) {
            UniValue ch(UniValue::VOBJ);
            ch.pushKV("kind", "mweb");
            ch.pushKV("address", w.mweb_bindings[0]->addr->mweb_address);
            ch.pushKV("amount_sat", op.mweb_send_change);
            ch.pushKV("label", "change");
            outputs_arr.push_back(ch);
        }

        UniValue balance_delta(UniValue::VOBJ);
        if (to_stealth) {
            // M→M: full amount + fee comes off MWEB.
            balance_delta.pushKV("canonical_sat", int64_t(0));
            balance_delta.pushKV("mweb_sat",
                                  -(op.mweb_send_amount + op.mweb_send_fee));
        } else {
            // Peg-out: amount leaves MWEB (lands on canonical bech32
            // after PEGOUT_MATURITY); fee burns from MWEB side.
            balance_delta.pushKV("canonical_sat", int64_t(0));
            balance_delta.pushKV("mweb_sat",
                                  -(op.mweb_send_amount + op.mweb_send_fee));
        }

        UniValue out(UniValue::VOBJ);
        out.pushKV("status", "estimated");
        out.pushKV("path", path_label);
        out.pushKV("txid", tx_id);
        out.pushKV("tx_hex", op.mweb_send_tx_hex);
        out.pushKV("amount_sat",         op.mweb_send_amount);
        out.pushKV("fee_sat",            op.mweb_send_fee);
        out.pushKV("fee_rate_sat_per_vb", int64_t(rate));
        out.pushKV("change_sat",         op.mweb_send_change);
        out.pushKV("inputs",             inputs_arr);
        out.pushKV("outputs",            outputs_arr);
        out.pushKV("balance_delta_sat",  balance_delta);
        out.pushKV("inputs_total_sat",   op.mweb_send_inputs_total);
        out.pushKV("inputs_count",       int64_t(op.mweb_send_inputs_commit.size()));
        op.result_json = out.write();
        op.phase = int32_t(MwebSendPhase::Done);
        op.state = OYO_OP_DONE;
        return;
    }

    UniValue params(UniValue::VARR);
    params.push_back(op.mweb_send_tx_hex);
    op.rpc_req = MakeRpc("oyo-mweb-send", "sendrawtransaction", params);
    op.phase   = int32_t(MwebSendPhase::WaitBroadcast);
    op.state   = OYO_OP_NEED_RPC;
}

void MwebSendHandleResp(OyoOpImpl& op, const UniValue& r) {
    if (!r.isStr()) throw std::runtime_error("sendrawtransaction did not return a txid");
    const std::string txid = LowerHex(r.get_str());

    // Proactively register pending-out markers on each selected confirmed
    // UTXO. The next mempool diff will reaffirm them; if the broadcast tx
    // never reaches anyone else's mempool the diff will roll them back.
    OyoChainImpl& c = *op.chain;
    for (const auto& commit : op.mweb_send_inputs_commit) {
        AddPendingMwebSpendLocked(c, commit);
    }

    UniValue out(UniValue::VOBJ);
    out.pushKV("status", "sent");
    out.pushKV("txid",   txid);
    out.pushKV("amount_sat",       op.mweb_send_amount);
    out.pushKV("fee_sat",          op.mweb_send_fee);
    out.pushKV("change_sat",       op.mweb_send_change);
    out.pushKV("inputs_total_sat", op.mweb_send_inputs_total);
    out.pushKV("inputs_count",     int64_t(op.mweb_send_inputs_commit.size()));
    op.result_json = out.write();
    op.phase = int32_t(MwebSendPhase::Done);
    op.state = OYO_OP_DONE;
}

void DispatchMwebSend(OyoOpImpl& op, const UniValue& env) {
    switch (MwebSendPhase(op.phase)) {
        case MwebSendPhase::WaitFeeRate: {
            try {
                op.fee_rate_sat_per_vb = ResolveFeeRateFromEstimateEnv(env);
            } catch (...) {
                op.fee_rate_sat_per_vb = kFallbackFeeRateSatPerVb;
            }
            FinalizeMwebSendOp(op);
            break;
        }
        case MwebSendPhase::WaitBroadcast: {
            UniValue r = UnwrapRpcResult(env);
            MwebSendHandleResp(op, r);
            break;
        }
        default: throw std::runtime_error("mweb_send response in unexpected phase");
    }
}

// ---------------------------------------------------------------------------
// Regular external (P2WPKH HD-derivation) send state machine.
//
// Inputs are picked largest-first across all the wallet's confirmed unspent
// UTXOs; outputs are recipient + change (back to address index 0). Each
// input is signed with SIGHASH_ALL via libbitcoin's MutableTransactionSig-
// natureCreator + ProduceSignature against a FillableSigningProvider seeded
// with the per-binding CKey re-derived from the wallet's oyo_v1 seed chain.
//
// Fee is a flat caller-supplied (or 2000 sat default) value — sufficient
// for a typical 1-in / 2-out tx at regtest's min relay (1 sat/vB ≈ 200 sat
// for our tx; we add slack for sigops and the BIP141 witness size). For
// mainnet a future slice should plug estimatesmartfee in.
//
// External_regular wallets cannot peg-in to an MWEB recipient from the
// canonical side (that requires a peg-in kernel + the peg-in pubkey, which
// the node-wallet builds via a special send path). We reject stealth
// destinations here with INVALID_ARG; users who want regular → MWEB should
// route through node-wallet (test-e2e style flow).
// ---------------------------------------------------------------------------

constexpr int64_t kP2WPKHDustSat            = 294;  // default min-relay dust for native segwit

// Conservative vsize constants for fee estimation. Exact post-sign vsize
// can be 1-2 vbytes lower (DER signature compaction) — we pay slightly
// over min-relay rather than risk under-paying.
constexpr uint64_t kTxOverheadVbytes      = 11;   // version + counts + locktime
constexpr uint64_t kP2WPKHInputVbytes     = 68;   // signed P2WPKH input
constexpr uint64_t kP2PKHInputVbytes      = 148;  // signed legacy P2PKH input (scriptSig, no witness)
constexpr uint64_t kP2shP2wpkhInputVbytes = 91;   // signed P2SH-P2WPKH (redeem in scriptSig + witness)
constexpr uint64_t kP2WPKHOutputVbytes    = 31;   // value + script_len + 22-byte script

uint64_t ScriptOutputVsize(const CScript& s) {
    return 8 + (s.size() < 253 ? 1 : 3) + s.size();
}

// Per-kind signed-input vsize for canonical inputs (P2.1 — a wallet may now
// hold p2wpkh / p2pkh / p2sh-p2wpkh UTXOs side by side, each a different size).
uint64_t CanonicalInputVbytes(const std::string& kind) {
    if (kind == "p2pkh")       return kP2PKHInputVbytes;
    if (kind == "p2sh-p2wpkh") return kP2shP2wpkhInputVbytes;
    return kP2WPKHInputVbytes;   // p2wpkh (and any default)
}

// Vsize for an N-input P2WPKH spend with the given output scripts.
// Used for fee sizing before signing — includes worst-case witness weight.
uint64_t EstimateP2wpkhSpendVsize(size_t n_inputs,
                                  const std::vector<CScript>& outputs) {
    uint64_t v = kTxOverheadVbytes + n_inputs * kP2WPKHInputVbytes;
    for (const auto& s : outputs) v += ScriptOutputVsize(s);
    return v;
}

struct RegInputCandidate {
    std::string  txid;
    uint32_t     vout       = 0;
    int64_t      amount_sat = 0;
    int          binding_index = 0;  // wallet-local Binding slot for key derivation
    std::string  script_hex;         // scriptPubKey hex of the input address
    std::string  kind;               // "p2wpkh" | "p2pkh" | "p2sh-p2wpkh"
};

// Pre-sign vsize for a mixed-kind canonical spend (P2.1): sum each picked
// input's per-kind vsize + the output scripts. Replaces the all-P2WPKH
// estimator at the call sites that can now see legacy / nested inputs.
uint64_t EstimateCanonicalSpendVsize(const std::vector<RegInputCandidate>& picked,
                                     const std::vector<CScript>& outputs) {
    uint64_t v = kTxOverheadVbytes;
    for (const auto& c : picked) v += CanonicalInputVbytes(c.kind);
    for (const auto& s : outputs) v += ScriptOutputVsize(s);
    return v;
}

// ---------------------------------------------------------------------------
// Branch-and-bound coin selection — port of Bitcoin/Litecoin Core's
// SelectCoinsBnB (wallet/coinselection.cpp). Searches for a subset of inputs
// whose total EFFECTIVE value (amount − input_fee) lands in
// [target, target + cost_of_change], minimising the excess. A hit means the
// leftover is small enough to fold into the fee instead of creating a change
// output — removing both the change vout AND the largest-first input-order
// fingerprint, matching node-wallet tx shape. The long-term-fee waste term is
// dropped (assumes current rate ≈ long-term rate), so "waste" reduces to the
// excess over target. Returns chosen candidate indices, or false → caller
// falls back to largest-first.
// ---------------------------------------------------------------------------
struct BnBCand { int64_t eff; size_t idx; };
bool SelectCoinsBnB(std::vector<BnBCand>& pool, int64_t target, int64_t cost_of_change,
                    std::vector<size_t>& out) {
    out.clear();
    if (target <= 0) return false;
    std::sort(pool.begin(), pool.end(),
              [](const BnBCand& a, const BnBCand& b) { return a.eff > b.eff; });
    int64_t total = 0;
    for (const auto& c : pool) total += c.eff;
    if (total < target) return false;

    std::vector<bool> sel;            // sel.size() == current depth in `pool`
    sel.reserve(pool.size());
    std::vector<bool> best;
    int64_t cur = 0;                  // sum of eff of currently-included
    int64_t avail = total;            // sum of eff of the undecided tail
    int64_t best_excess = INT64_MAX;
    const size_t kMaxTries = 100000;

    for (size_t tries = 0; tries < kMaxTries; ++tries) {
        bool backtrack = false;
        if (cur + avail < target ||           // cannot reach target down this branch
            cur > target + cost_of_change) {   // overshot the change window
            backtrack = true;
        } else if (cur >= target) {            // within [target, target+coc]
            int64_t excess = cur - target;
            if (excess < best_excess) { best = sel; best.resize(pool.size()); best_excess = excess; }
            backtrack = true;
        }
        if (backtrack) {
            // Pop the decided-excluded tail (returning them to `avail`), then
            // flip the last included → excluded.
            while (!sel.empty() && !sel.back()) {
                sel.pop_back();
                avail += pool[sel.size()].eff;
            }
            if (sel.empty()) break;            // search space exhausted
            sel.back() = false;
            cur -= pool[sel.size() - 1].eff;
        } else {
            // Decide pool[sel.size()]: include it — but skip an equal-value
            // exclude sibling to avoid exploring duplicate combinations.
            int64_t v = pool[sel.size()].eff;
            avail -= v;
            if (!sel.empty() && !sel.back() && v == pool[sel.size() - 1].eff) {
                sel.push_back(false);
            } else {
                sel.push_back(true);
                cur += v;
            }
        }
    }
    if (best.empty()) return false;
    for (size_t i = 0; i < best.size(); ++i) if (best[i]) out.push_back(pool[i].idx);
    return true;
}

void FinalizeRegularSendOp(OyoOpImpl& op);

void StartRegularSendOp(OyoOpImpl& op) {
    op.phase = int32_t(RegularSendPhase::Init);
    OyoWalletImpl& w = *op.wallet;
    if (!w.has_p2wpkh || w.watch_only) {
        throw std::runtime_error("wallet has no p2wpkh side");
    }
    if (w.seed_type != "oyo_v1" || w.address_kind != "p2wpkh") {
        throw std::runtime_error("only oyo_v1 p2wpkh wallets supported");
    }
    if (op.reg_send_to.empty()) {
        throw std::runtime_error("missing recipient address");
    }
    if (!op.send_all && op.reg_send_amount <= 0) {
        throw std::runtime_error("amount_sat must be > 0 (or set send_all=true)");
    }
    if (w.bindings.empty()) {
        throw std::runtime_error("wallet has no bindings");
    }

    CTxDestination dest = DecodeDestination(op.reg_send_to);
    if (!IsValidDestination(dest)) {
        throw std::runtime_error("invalid destination: " + op.reg_send_to);
    }
    if (boost::get<StealthAddress>(&dest)) {
        throw std::runtime_error("regular-external cannot send to MWEB stealth addresses; use a universal wallet");
    }

    // Caller-supplied fee_rate skips the estimatesmartfee roundtrip. Common
    // for the dry-run/estimate flow where the caller has already resolved
    // the rate once and reuses it across multiple estimate queries.
    if (op.fee_rate_sat_per_vb > 0) {
        FinalizeRegularSendOp(op);
        return;
    }
    EmitEstimateSmartFeeRpc(op);
    op.phase = int32_t(RegularSendPhase::WaitFeeRate);
}

void FinalizeRegularSendOp(OyoOpImpl& op) {
    OyoWalletImpl& w = *op.wallet;
    OyoChainImpl& c = *op.chain;
    const uint64_t rate = op.fee_rate_sat_per_vb > 0
        ? op.fee_rate_sat_per_vb
        : kFallbackFeeRateSatPerVb;

    // Recipient list — single legacy entry (op.reg_send_to + amount) or
    // the multi-recipient cfg.outputs[]. Both are validated all-canonical
    // by the impl_send entry; vsize estimate scales with N outputs.
    struct RegRecipient { CScript script; int64_t amount_sat; std::string address; };
    std::vector<RegRecipient> recipients;
    int64_t recipients_total = 0;
    if (op.send_outputs.empty()) {
        CTxDestination d0 = DecodeDestination(op.reg_send_to);
        recipients.push_back({GetScriptForDestination(d0), op.reg_send_amount, op.reg_send_to});
    } else {
        for (const auto& o : op.send_outputs) {
            CTxDestination d = DecodeDestination(o.address);
            if (!IsValidDestination(d) || boost::get<StealthAddress>(&d) != nullptr) {
                throw std::runtime_error("invalid canonical recipient: " + o.address);
            }
            recipients.push_back({GetScriptForDestination(d), o.amount_sat, o.address});
            recipients_total += o.amount_sat;
        }
    }
    // P2.4: index of the drain ("max") output among recipients, if any.
    // recipients[] preserves send_outputs order so the indices line up.
    // recipients_total already excludes it (its amount_sat is 0).
    int drain_idx = -1;
    for (size_t i = 0; i < op.send_outputs.size(); ++i)
        if (op.send_outputs[i].drain) { drain_idx = static_cast<int>(i); break; }

    // Single-recipient back-compat for the rest of the function — vsize
    // estimator wants a vector<CScript>.
    CScript recipient_script = recipients[0].script;
    // Placeholder P2WPKH script for fee-sizing change vout.
    CScript change_placeholder = CScript() << OP_0 << std::vector<uint8_t>(20, 0);

    std::vector<RegInputCandidate> all;
    for (const auto& b : w.bindings) {
        if (!b) continue;
        const Address& a = *b->addr;
        for (const auto& kv : a.utxos) {
            const Utxo& u = kv.second;
            if (!IsSpendableUtxoLocked(u, c.tip_height)) continue;
            RegInputCandidate cand;
            cand.txid          = u.txid;
            cand.vout          = u.vout;
            cand.amount_sat    = u.amount_sat;
            cand.binding_index = b->index;
            cand.script_hex    = a.script_hex;
            cand.kind          = a.kind;   // p2wpkh | p2pkh | p2sh-p2wpkh (P2.1)
            all.push_back(std::move(cand));
        }
    }
    std::sort(all.begin(), all.end(),
              [](const RegInputCandidate& x, const RegInputCandidate& y) {
                  return x.amount_sat > y.amount_sat;
              });

    // Manual input selection — if cfg.inputs[] was supplied, replace
    // `all` with only those outpoints (validated against the wallet's
    // current spendable set). Auto-select then picks from the manual
    // list; for non-send_all the loop will keep picking until needed
    // is covered, falling back to "use them all" semantics naturally.
    if (!op.send_inputs.empty()) {
        std::unordered_map<std::string, const RegInputCandidate*> by_outpoint;
        by_outpoint.reserve(all.size());
        for (const auto& cand : all) {
            by_outpoint[cand.txid + ":" + std::to_string(cand.vout)] = &cand;
        }
        std::vector<RegInputCandidate> manual;
        manual.reserve(op.send_inputs.size());
        for (const auto& s : op.send_inputs) {
            const std::string key = s.txid + ":" + std::to_string(s.vout);
            auto it = by_outpoint.find(key);
            if (it == by_outpoint.end()) {
                throw std::runtime_error(
                    "input not in wallet spendable UTXO set: " + key);
            }
            manual.push_back(*it->second);
        }
        // Sort largest-first so the auto-select branch below behaves
        // the same shape regardless of caller order. send_all drains
        // the whole list; targeted send picks just enough.
        std::sort(manual.begin(), manual.end(),
                  [](const RegInputCandidate& x, const RegInputCandidate& y) {
                      return x.amount_sat > y.amount_sat;
                  });
        all = std::move(manual);
    }

    // Effective recipient amount as a single number for legacy fields:
    // sum of multi-recipient amounts, or the single op.reg_send_amount.
    int64_t recipient_amount = op.send_outputs.empty()
        ? op.reg_send_amount : recipients_total;
    int64_t fee = 0;
    int64_t change = 0;
    int64_t total = 0;
    std::vector<RegInputCandidate> picked;

    if (op.send_all) {
        // send_all is only valid in single-recipient mode (rejected at
        // parse time when outputs[] is present). Drain all confirmed
        // UTXOs; recipient gets total - fee, no change vout.
        if (all.empty()) throw std::runtime_error("no confirmed UTXOs to spend");
        picked = all;
        for (const auto& cand : picked) total += cand.amount_sat;
        std::vector<CScript> outs{recipient_script};
        uint64_t vsize = EstimateCanonicalSpendVsize(picked, outs);
        fee = static_cast<int64_t>(vsize * rate);
        if (total <= fee) {
            throw std::runtime_error("insufficient funds: total=" + std::to_string(total) +
                                      " fee=" + std::to_string(fee));
        }
        recipient_amount = total - fee;
        change = 0;
    } else if (drain_idx >= 0) {
        // P2.4: the "max" output absorbs the remainder. Spend all selected
        // inputs (all confirmed, or the manual set), pay the fixed outputs,
        // and route everything left (minus fee) to the drain output — no
        // wallet change vout.
        if (all.empty()) throw std::runtime_error("no confirmed UTXOs to spend");
        picked = all;
        for (const auto& cand : picked) total += cand.amount_sat;
        const int64_t fixed_total = recipients_total;  // drain entry is 0
        std::vector<CScript> outs;
        outs.reserve(recipients.size());
        for (const auto& r : recipients) outs.push_back(r.script);
        uint64_t vsize = EstimateCanonicalSpendVsize(picked, outs);
        fee = static_cast<int64_t>(vsize * rate);
        const int64_t drain_amount = total - fixed_total - fee;
        if (drain_amount < kP2WPKHDustSat) {
            throw std::runtime_error(
                "insufficient funds: max output below dust after fixed outputs + fee");
        }
        recipients[drain_idx].amount_sat = drain_amount;
        recipient_amount = fixed_total + drain_amount;
        change = 0;
    } else {
        // Targeted send. Try branch-and-bound first (auto-select only): if a
        // subset matches the target within the cost-of-change window, spend it
        // with NO change vout — matching node-wallet's SelectCoinsBnB and
        // removing the largest-first fingerprint. Manual-input sends keep the
        // caller's set; BnB is skipped for them.
        bool bnb_done = false;
        if (op.send_inputs.empty() && !all.empty()) {
            std::vector<CScript> recip_scripts;
            recip_scripts.reserve(recipients.size());
            for (const auto& r : recipients) recip_scripts.push_back(r.script);
            // not_input_fees: overhead + recipient outputs, no inputs, no change.
            const int64_t base_fee = int64_t(EstimateCanonicalSpendVsize({}, recip_scripts) * rate);
            // cost of creating a change output now + spending it later (P2WPKH).
            const int64_t coc = int64_t((kP2WPKHOutputVbytes + kP2WPKHInputVbytes) * rate);
            const int64_t bnb_target = recipient_amount + base_fee;
            std::vector<BnBCand> pool;
            pool.reserve(all.size());
            for (size_t i = 0; i < all.size(); ++i) {
                int64_t in_fee = int64_t(CanonicalInputVbytes(all[i].kind) * rate);
                int64_t eff = all[i].amount_sat - in_fee;
                if (eff <= 0) continue;          // uneconomic input — exclude (as Core does)
                pool.push_back({eff, i});
            }
            std::vector<size_t> chosen;
            if (SelectCoinsBnB(pool, bnb_target, coc, chosen)) {
                for (size_t idx : chosen) { picked.push_back(all[idx]); total += all[idx].amount_sat; }
                // The excess over the minimal fee is ≤ coc by construction —
                // fold it into the fee instead of a dust change vout.
                fee = total - recipient_amount;
                change = 0;
                bnb_done = true;
            }
        }
        if (!bnb_done) {
            // Fallback: pick smallest-N largest-first to cover (recipients_total + fee).
            // Fee depends on N inputs and M+1 outputs (M recipients + change),
            // so we re-estimate after each pick. With manual inputs, `all`
            // already contains only the user's chosen UTXOs.
            std::vector<CScript> outs2;
            outs2.reserve(recipients.size() + 1);
            for (const auto& r : recipients) outs2.push_back(r.script);
            outs2.push_back(change_placeholder);
            int64_t needed = recipient_amount;  // adjusted below
            for (const auto& cand : all) {
                if (!picked.empty() && total >= needed) break;
                picked.push_back(cand);
                total += cand.amount_sat;
                uint64_t vs = EstimateCanonicalSpendVsize(picked, outs2);
                fee = static_cast<int64_t>(vs * rate);
                needed = recipient_amount + fee;
            }
            // With manual inputs we use ALL of them (caller's intent —
            // they explicitly picked these). Auto-select stops as soon as
            // it's covered; manual must consume every listed outpoint.
            if (!op.send_inputs.empty() && picked.size() < all.size()) {
                for (size_t i = picked.size(); i < all.size(); ++i) {
                    picked.push_back(all[i]);
                    total += all[i].amount_sat;
                }
                uint64_t vs = EstimateCanonicalSpendVsize(picked, outs2);
                fee = static_cast<int64_t>(vs * rate);
                needed = recipient_amount + fee;
            }
            if (total < needed) {
                throw std::runtime_error("insufficient funds: need=" + std::to_string(needed) +
                                          " available=" + std::to_string(total));
            }
            change = total - recipient_amount - fee;
            if (change < 0) {
                throw std::runtime_error("internal: regular-send change underflow");
            }
            if (change > 0 && change < kP2WPKHDustSat) {
                // Below dust → fold into fee, drop the change vout. Re-estimate
                // vsize for the M-output tx (no change): shaves ~31 vbytes.
                std::vector<CScript> outs1;
                outs1.reserve(recipients.size());
                for (const auto& r : recipients) outs1.push_back(r.script);
                uint64_t vs1 = EstimateCanonicalSpendVsize(picked, outs1);
                int64_t fee1 = static_cast<int64_t>(vs1 * rate);
                fee = (total - recipient_amount);  // absorb everything into fee
                (void)fee1;  // informational; we cap fee to leftover
                change = 0;
            }
        }
    }

    op.reg_send_amount        = recipient_amount;
    op.reg_send_fee           = fee;
    op.reg_send_change        = change;
    op.reg_send_inputs_total  = total;

    // Shuffle picked inputs (parity with node-wallet AddTxInputs which
    // calls Shuffle on selected_coins before constructing vin). Eliminates
    // the "largest-first input order" wallet fingerprint.
    {
        FastRandomContext rng;
        std::shuffle(picked.begin(), picked.end(), rng);
    }

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::CURRENT_VERSION;
    mtx.nLockTime = AntiFeeSnipingLocktime(c);
    op.reg_send_inputs_outpoints.clear();
    op.reg_send_inputs_outpoints.reserve(picked.size());
    for (const auto& cand : picked) {
        uint256 txid_hash;
        txid_hash.SetHex(cand.txid);
        if (txid_hash.IsNull() && cand.txid != std::string(64, '0')) {
            throw std::runtime_error("invalid utxo txid: " + cand.txid);
        }
        // Match Litecoin Core's wallet default: DEFAULT_WALLET_RBF=false →
        // sequence = SEQUENCE_FINAL - 1 (0xfffffffe). Bitcoin Core defaults
        // RBF on; Litecoin defaults it off, so for fingerprint parity we
        // mirror the litecoind side (txassembler.cpp:338).
        mtx.vin.emplace_back(CTxIn(COutPoint(txid_hash, cand.vout),
                                    CScript(), CTxIn::SEQUENCE_FINAL - 1));
        op.reg_send_inputs_outpoints.push_back(cand.txid + ":" + std::to_string(cand.vout));
    }
    // Emit one CTxOut per recipient. send_all stays single-recipient
    // (recipients[0] only); multi-recipient hits the targeted-send
    // branch. In send_all mode recipients[0].amount_sat was 0 (not yet
    // set) — substitute the drained recipient_amount.
    if (op.send_all) {
        mtx.vout.emplace_back(CTxOut(recipient_amount, recipients[0].script));
    } else {
        for (const auto& r : recipients) {
            mtx.vout.emplace_back(CTxOut(r.amount_sat, r.script));
        }
    }

    // Change to a fresh HD-derived P2WPKH binding (privacy: never reuse
    // the same change address). Inserted at a random position via
    // GetRandInt(N+1) so the change-vout can't be identified by an
    // always-last fingerprint (matches node-wallet txassembler.cpp:156-158).
    //
    // Allocated even in dry_run: the confirm-token flow broadcasts the
    // dry-run-built signed tx VERBATIM, so the change must land on a real,
    // wallet-owned address here. The earlier all-zero placeholder was safe
    // only while dry-run output was discarded; once it became the broadcast
    // tx, a placeholder burned the entire change to OP_0<20*0x00>. The
    // binding is registered so chain-sync/rescan reattributes the change.
    if (change > 0) {
        CScript change_script;
        if (!op.change_address.empty()) {
            // P2.3: caller-specified change destination. Validated canonical
            // at parse time; build its script directly. No binding alloc —
            // it may be an external address the wallet doesn't own (the user
            // explicitly chose where the change goes).
            CTxDestination cd = DecodeDestination(op.change_address);
            if (!IsValidDestination(cd)) {
                throw std::runtime_error("custom change_address decode failed");
            }
            change_script = GetScriptForDestination(cd);
        } else {
            int change_idx = AllocateP2wpkhBindingLocked(c, w, /*runtime_alloc=*/true);
            if (change_idx < 0) {
                throw std::runtime_error("change-binding allocation failed");
            }
            const Address* a = w.bindings[change_idx]->addr;
            CTxDestination cd = DecodeDestination(a->address);
            if (!IsValidDestination(cd)) {
                throw std::runtime_error("fresh change address decode failed");
            }
            change_script = GetScriptForDestination(cd);
        }
        CTxOut change_out(change, change_script);
        const int pos = GetRandInt(static_cast<int>(mtx.vout.size()) + 1);
        mtx.vout.insert(mtx.vout.begin() + pos, change_out);
    }

    // Sign each input with its derived key. dry_run still signs (so the
    // serialized tx is broadcast-ready if the caller wants to reuse it).
    FillableSigningProvider keystore;
    const std::string net = c.network.empty() ? std::string("regtest") : c.network;
    for (const auto& cand : picked) {
        CKey key;
        if (!DeriveOyoV1Key(w.seed_bytes, net, cand.binding_index, key)) {
            throw std::runtime_error("key derivation failed for binding " +
                                      std::to_string(cand.binding_index));
        }
        keystore.AddKey(key);
        // P2SH-P2WPKH (P2.1): ProduceSignature needs the redeem script
        // (the wrapped P2WPKH script) to build scriptSig=<redeem> + witness.
        // P2PKH / P2WPKH need only the key.
        if (cand.kind == "p2sh-p2wpkh") {
            keystore.AddCScript(GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey())));
        }
    }
    for (size_t i = 0; i < mtx.vin.size(); ++i) {
        SignatureData sigdata;
        const auto& cand = picked[i];
        std::vector<uint8_t> script_bytes = ParseHex(cand.script_hex);
        CScript prevScript(script_bytes.begin(), script_bytes.end());

        MutableTransactionSignatureCreator creator(
            &mtx, static_cast<unsigned int>(i),
            cand.amount_sat, SIGHASH_ALL);
        if (!ProduceSignature(keystore, creator, prevScript, sigdata)) {
            throw std::runtime_error("signing failed for input " + std::to_string(i));
        }
        UpdateInput(mtx.vin[i], sigdata);
    }

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << mtx;
    op.reg_send_tx_hex = HexStr(ss);
    const std::string tx_id = mtx.GetHash().GetHex();

    if (op.dry_run) {
        // Structured tx breakdown for the pre-broadcast confirmation
        // modal — frontend renders inputs/outputs/path/balance-delta from
        // these fields and uses tx_id as the cache token to broadcast the
        // signed hex on Confirm.
        UniValue inputs_arr(UniValue::VARR);
        for (const auto& outpoint : op.reg_send_inputs_outpoints) {
            auto colon = outpoint.find(':');
            if (colon == std::string::npos) continue;
            UtxoKey key{outpoint.substr(0, colon),
                        static_cast<uint32_t>(std::stoul(outpoint.substr(colon + 1)))};
            for (const auto& b : w.bindings) {
                if (!b) continue;
                auto uit = b->addr->utxos.find(key);
                if (uit == b->addr->utxos.end()) continue;
                UniValue ent(UniValue::VOBJ);
                ent.pushKV("kind", b->addr->kind);
                ent.pushKV("txid", key.txid);
                ent.pushKV("vout", int64_t(key.vout));
                ent.pushKV("amount_sat", uit->second.amount_sat);
                ent.pushKV("address", b->addr->address);
                ent.pushKV("binding_index", b->index);
                ent.pushKV("status",
                           IsImmatureUtxoLocked(uit->second, c.tip_height)
                               ? "immature"
                               : (uit->second.confirmed ? "confirmed" : "pending"));
                inputs_arr.push_back(ent);
                break;
            }
        }
        // Recipient label set: any vout matching one of the configured
        // recipient addresses is "recipient"; the leftover (always at most
        // one, the change vout we just inserted) is "change".
        std::unordered_set<std::string> recipient_addrs;
        for (const auto& r : recipients) recipient_addrs.insert(r.address);
        UniValue outputs_arr(UniValue::VARR);
        for (size_t i = 0; i < mtx.vout.size(); ++i) {
            const CTxOut& o = mtx.vout[i];
            CTxDestination dest;
            std::string addr;
            if (ExtractDestination(o.scriptPubKey, dest) && IsValidDestination(dest)) {
                addr = EncodeDestination(dest);
            }
            UniValue ent(UniValue::VOBJ);
            ent.pushKV("kind", "p2wpkh");
            ent.pushKV("address", addr);
            ent.pushKV("amount_sat", int64_t(o.nValue));
            ent.pushKV("label",
                        recipient_addrs.count(addr) ? "recipient" : "change");
            outputs_arr.push_back(ent);
        }
        UniValue balance_delta(UniValue::VOBJ);
        balance_delta.pushKV("canonical_sat",
                              -(op.reg_send_amount + op.reg_send_fee));
        balance_delta.pushKV("mweb_sat", int64_t(0));

        UniValue out(UniValue::VOBJ);
        out.pushKV("status", "estimated");
        out.pushKV("path",   "regular");
        out.pushKV("txid",   tx_id);
        out.pushKV("tx_hex", op.reg_send_tx_hex);
        out.pushKV("amount_sat",         op.reg_send_amount);
        out.pushKV("fee_sat",            op.reg_send_fee);
        out.pushKV("fee_rate_sat_per_vb", int64_t(rate));
        out.pushKV("change_sat",         op.reg_send_change);
        out.pushKV("inputs",             inputs_arr);
        out.pushKV("outputs",            outputs_arr);
        out.pushKV("balance_delta_sat",  balance_delta);
        out.pushKV("inputs_total_sat",   op.reg_send_inputs_total);
        out.pushKV("inputs_count",       int64_t(op.reg_send_inputs_outpoints.size()));
        op.result_json = out.write();
        op.phase = int32_t(RegularSendPhase::Done);
        op.state = OYO_OP_DONE;
        return;
    }

    UniValue params(UniValue::VARR);
    params.push_back(op.reg_send_tx_hex);
    op.rpc_req = MakeRpc("oyo-reg-send", "sendrawtransaction", params);
    op.phase   = int32_t(RegularSendPhase::WaitBroadcast);
    op.state   = OYO_OP_NEED_RPC;
}

void RegularSendHandleResp(OyoOpImpl& op, const UniValue& r) {
    if (!r.isStr()) throw std::runtime_error("sendrawtransaction did not return a txid");
    const std::string txid = LowerHex(r.get_str());

    // Optimistically mark each consumed UTXO as pending-out so the wallet
    // status updates before the next chain.sync. The mempool sync will
    // reaffirm or roll back. Walk our bindings and mark by (txid, vout).
    OyoChainImpl& c = *op.chain;
    OyoWalletImpl& w = *op.wallet;
    for (const auto& outpoint : op.reg_send_inputs_outpoints) {
        auto colon = outpoint.find(':');
        if (colon == std::string::npos) continue;
        UtxoKey key{outpoint.substr(0, colon),
                    static_cast<uint32_t>(std::stoul(outpoint.substr(colon + 1)))};
        for (const auto& b : w.bindings) {
            if (!b) continue;
            auto it = b->addr->utxos.find(key);
            if (it != b->addr->utxos.end() && !it->second.spent_pending) {
                Utxo& u = it->second;
                u.spent_pending = true;
                u.spent_pending_txid = txid;
                if (u.confirmed && !u.spent) {
                    b->addr->confirmed_sat   -= u.amount_sat;
                    b->addr->pending_out_sat += u.amount_sat;
                    NotifyWatchersLocked(*b->addr, -u.amount_sat);
                }
                break;
            }
        }
    }
    (void)c; // chain.mempool diff will materialise the canonical pending entry.

    UniValue out(UniValue::VOBJ);
    out.pushKV("status", "sent");
    out.pushKV("txid",   txid);
    out.pushKV("amount_sat",       op.reg_send_amount);
    out.pushKV("fee_sat",          op.reg_send_fee);
    out.pushKV("change_sat",       op.reg_send_change);
    out.pushKV("inputs_total_sat", op.reg_send_inputs_total);
    out.pushKV("inputs_count",     int64_t(op.reg_send_inputs_outpoints.size()));
    op.result_json = out.write();
    op.phase = int32_t(RegularSendPhase::Done);
    op.state = OYO_OP_DONE;
}

void DispatchRegularSend(OyoOpImpl& op, const UniValue& env) {
    switch (RegularSendPhase(op.phase)) {
        case RegularSendPhase::WaitFeeRate: {
            // estimatesmartfee may return an error envelope when the
            // estimator has no data (regtest, fresh node) — treat that
            // as fallback rate, don't propagate the error.
            try {
                UniValue r = UnwrapRpcResult(env);
                op.fee_rate_sat_per_vb = ResolveFeeRateFromEstimateEnv(env);
                (void)r;
            } catch (...) {
                op.fee_rate_sat_per_vb = kFallbackFeeRateSatPerVb;
            }
            FinalizeRegularSendOp(op);
            break;
        }
        case RegularSendPhase::WaitBroadcast: {
            UniValue r = UnwrapRpcResult(env);
            RegularSendHandleResp(op, r);
            break;
        }
        default: throw std::runtime_error("regular_send response in unexpected phase");
    }
}

// ---------------------------------------------------------------------------
// External peg-in (R→M) state machine.
//
// Universal-wallet only: requires both has_p2wpkh (canonical inputs &
// canonical change) and has_mweb (because the MWEB side of the resulting
// tx is rewound by libmw against the wallet's keychain — the recipient is
// some external stealth, but the wallet itself must own a keychain to
// receive future change/refunds; we additionally tolerate the recipient
// being one of our own stealth addresses for self-test purposes).
//
// Fee model:
//   mweb_fee  = mweb_weight * 100  (=2100 for 1 stealth recv + 1 kernel)
//   canonical_fee = caller's fee_sat - mweb_fee
//   pegin_amount = recipient_amount + mweb_fee
// Caller's fee_sat must cover both halves; default 5000 from the ABI.
// ---------------------------------------------------------------------------

struct ExtPegInInputCandidate {
    std::string txid;
    uint32_t    vout       = 0;
    int64_t     amount_sat = 0;
    int         binding_index = 0;
    std::string script_hex;
};

// overhead + peg-in vout + the 164 weight (= 41 vbytes) that
// consensus/validation.h's GetTransactionWeight adds for each PegInCoin
// in mweb_tx. Without that 41 the node rejects send_all peg-ins with
// "min relay fee not met" because mweb_fee alone can't absorb the gap.
constexpr uint64_t kExtPegInBaseCanonicalVbytes = kTxOverheadVbytes + 43 + 41;
constexpr uint64_t kMwebFeeWithChange = 39 * 100;  // 1 recv + 1 change + kernel-w-stealth
constexpr uint64_t kMwebFeeNoChange   = 21 * 100;  // 1 recv + kernel-w-stealth

void FinalizeExtPegInOp(OyoOpImpl& op);

void StartExtPegInOp(OyoOpImpl& op) {
    op.phase = int32_t(ExtPegInPhase::Init);
    OyoWalletImpl& w = *op.wallet;
    // Peg-in needs a P2WPKH side (we spend canonical inputs to fund the
    // pegin kernel). The MWEB side is optional: when present, change goes
    // back as own MWEB output (privacy parity with node default); when
    // absent, change stays on the canonical side (matches node behaviour
    // when coin_control.destChange is a non-stealth address).
    if (!w.has_p2wpkh || w.watch_only) {
        throw std::runtime_error("peg-in requires a wallet with p2wpkh side (regular or universal)");
    }
    if (w.seed_type != "oyo_v1") {
        throw std::runtime_error("peg-in supports only oyo_v1 wallets");
    }
    if (op.ext_pegin_to.empty()) {
        throw std::runtime_error("missing recipient address");
    }
    if (!op.send_all && op.ext_pegin_amount <= 0) {
        throw std::runtime_error("amount_sat must be > 0 (or set send_all=true)");
    }
    if (w.bindings.empty()) {
        throw std::runtime_error("wallet has no canonical bindings");
    }

    CTxDestination dest = DecodeDestination(op.ext_pegin_to);
    if (!IsValidDestination(dest)) {
        throw std::runtime_error("invalid destination: " + op.ext_pegin_to);
    }
    if (!boost::get<StealthAddress>(&dest)) {
        throw std::runtime_error("peg-in destination must be a stealth (MWEB) address");
    }

    if (op.fee_rate_sat_per_vb > 0) {
        FinalizeExtPegInOp(op);
        return;
    }
    EmitEstimateSmartFeeRpc(op);
    op.phase = int32_t(ExtPegInPhase::WaitFeeRate);
}

void FinalizeExtPegInOp(OyoOpImpl& op) {
    OyoWalletImpl& w = *op.wallet;
    OyoChainImpl& c = *op.chain;
    const uint64_t rate = op.fee_rate_sat_per_vb > 0
        ? op.fee_rate_sat_per_vb
        : kFallbackFeeRateSatPerVb;

    // Privacy parity with node default: when the wallet has its own MWEB
    // side, send change back as an own MWEB output (matches libmw
    // CHANGE_INDEX). When the wallet is canonical-only (regular), change
    // goes back as a fresh P2WPKH vout — same as node behaviour when
    // coin_control.destChange is a non-stealth address.
    const bool change_on_mweb = w.has_mweb && !w.mweb_bindings.empty()
                                && w.mweb_bindings[0] && w.mweb_bindings[0]->addr;

    std::vector<ExtPegInInputCandidate> all;
    for (const auto& b : w.bindings) {
        if (!b) continue;
        const Address& a = *b->addr;
        // Peg-in (R→M) spends only native P2WPKH inputs — the canonical
        // change + vsize path here assume P2WPKH. Legacy / nested-SegWit
        // UTXOs (P2.1) are spent via regular sends instead, so skip them
        // rather than mis-size the fee or fail signing without their redeem.
        if (!a.kind.empty() && a.kind != "p2wpkh") continue;
        for (const auto& kv : a.utxos) {
            const Utxo& u = kv.second;
            if (!IsSpendableUtxoLocked(u, c.tip_height)) continue;
            ExtPegInInputCandidate cand;
            cand.txid          = u.txid;
            cand.vout          = u.vout;
            cand.amount_sat    = u.amount_sat;
            cand.binding_index = b->index;
            cand.script_hex    = a.script_hex;
            all.push_back(std::move(cand));
        }
    }
    std::sort(all.begin(), all.end(),
              [](const ExtPegInInputCandidate& x, const ExtPegInInputCandidate& y) {
                  return x.amount_sat > y.amount_sat;
              });

    // Manual input selection — replace `all` with only the caller's
    // canonical outpoints (validated against the wallet's spendable
    // P2WPKH set).
    if (!op.send_inputs.empty()) {
        std::unordered_map<std::string, const ExtPegInInputCandidate*> by_outpoint;
        by_outpoint.reserve(all.size());
        for (const auto& cand : all) {
            by_outpoint[cand.txid + ":" + std::to_string(cand.vout)] = &cand;
        }
        std::vector<ExtPegInInputCandidate> manual;
        manual.reserve(op.send_inputs.size());
        for (const auto& s : op.send_inputs) {
            const std::string key = s.txid + ":" + std::to_string(s.vout);
            auto it = by_outpoint.find(key);
            if (it == by_outpoint.end()) {
                throw std::runtime_error(
                    "input not in wallet spendable UTXO set: " + key);
            }
            manual.push_back(*it->second);
        }
        std::sort(manual.begin(), manual.end(),
                  [](const ExtPegInInputCandidate& x, const ExtPegInInputCandidate& y) {
                      return x.amount_sat > y.amount_sat;
                  });
        all = std::move(manual);
    }

    // Coin selection. canonical_vsize includes a P2WPKH change vout iff
    // change_on_mweb is false (and we still expect change). When MWEB
    // hosts the change, the canonical side has only the peg-in vout.
    auto canonical_vsize = [&](size_t n_inputs, bool with_change_vout) -> uint64_t {
        uint64_t v = kExtPegInBaseCanonicalVbytes + n_inputs * kP2WPKHInputVbytes;
        if (with_change_vout) v += kP2WPKHOutputVbytes;
        return v;
    };
    // The reservation budget assumes change-on-canonical takes a vout when
    // change_on_mweb=false. mweb_fee is fixed-per-mode at selection time:
    // 3900 when MWEB hosts change, 2100 otherwise.
    const uint64_t selection_mweb_fee = change_on_mweb ? kMwebFeeWithChange
                                                       : kMwebFeeNoChange;

    std::vector<ExtPegInInputCandidate> picked;
    int64_t total = 0;

    if (op.send_all) {
        if (all.empty()) throw std::runtime_error("no confirmed UTXOs to spend");
        picked = all;
        for (const auto& cand : picked) total += cand.amount_sat;
    } else {
        // Pick smallest-N largest-first to cover
        // (recipient + mweb_fee + canonical_fee_assuming_change).
        const bool reserve_change = !change_on_mweb;
        int64_t needed = op.ext_pegin_amount + int64_t(selection_mweb_fee);
        for (const auto& cand : all) {
            if (!picked.empty() && total >= needed) break;
            picked.push_back(cand);
            total += cand.amount_sat;
            int64_t cf = int64_t(canonical_vsize(picked.size(), reserve_change) * rate);
            needed = op.ext_pegin_amount + int64_t(selection_mweb_fee) + cf;
        }
        // Manual inputs: consume every specified outpoint, even if the
        // targeted-send loop already covered `needed`. Caller picked
        // exactly these.
        if (!op.send_inputs.empty() && picked.size() < all.size()) {
            for (size_t i = picked.size(); i < all.size(); ++i) {
                picked.push_back(all[i]);
                total += all[i].amount_sat;
            }
        }
        if (total < needed) {
            throw std::runtime_error("insufficient funds: need=" + std::to_string(needed) +
                                      " available=" + std::to_string(total));
        }
    }
    op.ext_pegin_inputs_total = total;

    // Compute fees + change for the picked inputs. When change_on_mweb,
    // canonical has no change vout (mweb absorbs leftover); otherwise we
    // start with a change vout and may fold-it-into-fee if dust/zero.
    int64_t canonical_fee = int64_t(canonical_vsize(picked.size(), !change_on_mweb) * rate);
    int64_t recipient_amount = op.ext_pegin_amount;
    int64_t mweb_change = 0;          // own-MWEB change (only when change_on_mweb)
    int64_t canonical_change = 0;     // own-P2WPKH change (only when !change_on_mweb)
    uint64_t mweb_fee_budget = selection_mweb_fee;

    if (op.send_all) {
        // Drain — no change on either side. mweb_fee always = 2100 here.
        mweb_fee_budget = kMwebFeeNoChange;
        canonical_fee = int64_t(canonical_vsize(picked.size(), false) * rate);
        if (total <= canonical_fee + int64_t(kMwebFeeNoChange)) {
            throw std::runtime_error("insufficient funds for fees in send_all");
        }
        recipient_amount = total - canonical_fee - int64_t(kMwebFeeNoChange);
    } else if (change_on_mweb) {
        mweb_change = total - op.ext_pegin_amount - int64_t(kMwebFeeWithChange) - canonical_fee;
        if (mweb_change < 0) {
            throw std::runtime_error("internal: mweb_change underflow");
        }
        if (mweb_change == 0) {
            // Exact selection — drop change recipient. Lighter mweb_fee.
            mweb_fee_budget = kMwebFeeNoChange;
            const int64_t savings = int64_t(kMwebFeeWithChange) - int64_t(kMwebFeeNoChange);
            canonical_fee += savings;
        }
    } else {
        // Canonical change branch (R wallet or U wallet without an mweb addr).
        canonical_change = total - op.ext_pegin_amount - int64_t(kMwebFeeNoChange) - canonical_fee;
        if (canonical_change < 0) {
            throw std::runtime_error("internal: canonical_change underflow");
        }
        if (canonical_change < kP2WPKHDustSat) {
            // Zero or sub-dust change → drop the change vout, fold leftover
            // into the canonical fee. Mirrors regular-send dust handling.
            canonical_fee = total - op.ext_pegin_amount - int64_t(kMwebFeeNoChange);
            canonical_change = 0;
        }
    }
    op.ext_pegin_canonical_fee   = canonical_fee;
    op.ext_pegin_change          = change_on_mweb ? mweb_change : canonical_change;
    op.ext_pegin_change_on_mweb  = change_on_mweb;
    op.ext_pegin_amount          = recipient_amount;

    const std::string mweb_change_addr = change_on_mweb
        ? w.mweb_bindings[0]->addr->mweb_address
        : std::string();
    auto mweb_built = oyoltc::mweb::BuildPegInMwebPart(
        op.ext_pegin_to,
        static_cast<uint64_t>(recipient_amount),
        mweb_change_addr,
        static_cast<uint64_t>(change_on_mweb ? mweb_change : 0),
        mweb_fee_budget);
    op.ext_pegin_mweb_fee = static_cast<int64_t>(mweb_built.actual_mweb_fee_sat);
    const int64_t pegin_amount = static_cast<int64_t>(mweb_built.pegin_amount_sat);

    // Shuffle picked inputs (parity with node-wallet AddTxInputs).
    {
        FastRandomContext rng;
        std::shuffle(picked.begin(), picked.end(), rng);
    }

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::CURRENT_VERSION;
    mtx.nLockTime = AntiFeeSnipingLocktime(c);

    op.ext_pegin_inputs_outpoints.clear();
    op.ext_pegin_inputs_outpoints.reserve(picked.size());
    for (const auto& cand : picked) {
        uint256 txid_hash;
        txid_hash.SetHex(cand.txid);
        if (txid_hash.IsNull() && cand.txid != std::string(64, '0')) {
            throw std::runtime_error("invalid utxo txid: " + cand.txid);
        }
        mtx.vin.emplace_back(CTxIn(COutPoint(txid_hash, cand.vout),
                                    CScript(), MAX_BIP125_RBF_SEQUENCE));
        op.ext_pegin_inputs_outpoints.push_back(cand.txid + ":" + std::to_string(cand.vout));
    }

    mw::Hash kernel_hash;
    {
        std::vector<uint8_t> kid(mweb_built.kernel_id.begin(), mweb_built.kernel_id.end());
        kernel_hash = mw::Hash(kid.data());
    }
    // Peg-in vout position is randomized via GetRandInt(N+1) — node-wallet
    // does the same in AddTxInputs. With change_on_mweb=true the canonical
    // side has only this vout, so randomization is a no-op; with canonical
    // change it interleaves the peg-in vout amongst the other vouts so the
    // change vout's position is not always-last.
    CScript change_script;
    bool need_change_vout = false;
    if (!op.send_all && !change_on_mweb && canonical_change > 0) {
        need_change_vout = true;
        // Allocated even in dry_run: confirm broadcasts the dry-run-built
        // signed tx verbatim, so canonical change must land on a real,
        // wallet-owned address — the old all-zero placeholder burned it.
        int change_idx = AllocateP2wpkhBindingLocked(c, w, /*runtime_alloc=*/true);
        if (change_idx < 0) {
            throw std::runtime_error("change-binding allocation failed");
        }
        const Address* a = w.bindings[change_idx]->addr;
        CTxDestination cd = DecodeDestination(a->address);
        if (!IsValidDestination(cd)) {
            throw std::runtime_error("fresh change address decode failed");
        }
        change_script = GetScriptForDestination(cd);
    }
    if (need_change_vout) {
        mtx.vout.emplace_back(CTxOut(canonical_change, change_script));
    }
    {
        const int pos = GetRandInt(static_cast<int>(mtx.vout.size()) + 1);
        mtx.vout.insert(mtx.vout.begin() + pos,
                        CTxOut(pegin_amount, GetScriptForPegin(kernel_hash)));
    }

    FillableSigningProvider keystore;
    const std::string net = c.network.empty() ? std::string("regtest") : c.network;
    for (const auto& cand : picked) {
        CKey key;
        if (!DeriveOyoV1Key(w.seed_bytes, net, cand.binding_index, key)) {
            throw std::runtime_error("key derivation failed for binding " +
                                      std::to_string(cand.binding_index));
        }
        keystore.AddKey(key);
    }
    for (size_t i = 0; i < picked.size(); ++i) {
        SignatureData sigdata;
        const auto& cand = picked[i];
        std::vector<uint8_t> script_bytes = ParseHex(cand.script_hex);
        CScript prevScript(script_bytes.begin(), script_bytes.end());

        MutableTransactionSignatureCreator creator(
            &mtx, static_cast<unsigned int>(i),
            cand.amount_sat, SIGHASH_ALL);
        if (!ProduceSignature(keystore, creator, prevScript, sigdata)) {
            throw std::runtime_error("signing failed for input " + std::to_string(i));
        }
        UpdateInput(mtx.vin[i], sigdata);
    }

    mtx.mweb_tx = MWEB::Tx(mweb_built.mweb_tx);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << mtx;
    op.ext_pegin_tx_hex = HexStr(ss);

    const std::string tx_id = mtx.GetHash().GetHex();

    if (op.dry_run) {
        // Inputs: canonical P2WPKH selected from the wallet's bindings.
        UniValue inputs_arr(UniValue::VARR);
        for (const auto& outpoint : op.ext_pegin_inputs_outpoints) {
            auto colon = outpoint.find(':');
            if (colon == std::string::npos) continue;
            UtxoKey key{outpoint.substr(0, colon),
                        static_cast<uint32_t>(std::stoul(outpoint.substr(colon + 1)))};
            for (const auto& b : w.bindings) {
                if (!b) continue;
                auto uit = b->addr->utxos.find(key);
                if (uit == b->addr->utxos.end()) continue;
                UniValue ent(UniValue::VOBJ);
                ent.pushKV("kind", b->addr->kind);
                ent.pushKV("txid", key.txid);
                ent.pushKV("vout", int64_t(key.vout));
                ent.pushKV("amount_sat", uit->second.amount_sat);
                ent.pushKV("address", b->addr->address);
                ent.pushKV("binding_index", b->index);
                ent.pushKV("status",
                           IsImmatureUtxoLocked(uit->second, c.tip_height)
                               ? "immature"
                               : (uit->second.confirmed ? "confirmed" : "pending"));
                inputs_arr.push_back(ent);
                break;
            }
        }
        // Outputs: walk canonical mtx.vout (kernel + maybe canonical
        // change), then append the MWEB recipient + maybe MWEB change.
        UniValue outputs_arr(UniValue::VARR);
        for (size_t i = 0; i < mtx.vout.size(); ++i) {
            const CTxOut& o = mtx.vout[i];
            CTxDestination dest;
            std::string addr;
            bool has_dest = ExtractDestination(o.scriptPubKey, dest) &&
                             IsValidDestination(dest);
            if (has_dest) addr = EncodeDestination(dest);
            UniValue ent(UniValue::VOBJ);
            // Peg-in kernel vout has scriptPubKey = OP_8 || kernel_id;
            // ExtractDestination doesn't recognize it. Treat unknown
            // scripts in this finalizer as the kernel vout.
            if (!has_dest) {
                ent.pushKV("kind", "kernel");
                ent.pushKV("address", "");
                ent.pushKV("amount_sat", int64_t(o.nValue));
                ent.pushKV("label", "kernel");
            } else {
                ent.pushKV("kind", "p2wpkh");
                ent.pushKV("address", addr);
                ent.pushKV("amount_sat", int64_t(o.nValue));
                ent.pushKV("label", "change");   // canonical-side vouts
            }
            outputs_arr.push_back(ent);
        }
        // MWEB recipient.
        {
            UniValue ent(UniValue::VOBJ);
            ent.pushKV("kind", "mweb");
            ent.pushKV("address", op.ext_pegin_to);
            ent.pushKV("amount_sat", op.ext_pegin_amount);
            ent.pushKV("label", "recipient");
            outputs_arr.push_back(ent);
        }
        if (change_on_mweb && op.ext_pegin_change > 0 &&
            !w.mweb_bindings.empty() && w.mweb_bindings[0] &&
            w.mweb_bindings[0]->addr) {
            UniValue ent(UniValue::VOBJ);
            ent.pushKV("kind", "mweb");
            ent.pushKV("address", w.mweb_bindings[0]->addr->mweb_address);
            ent.pushKV("amount_sat", op.ext_pegin_change);
            ent.pushKV("label", "change");
            outputs_arr.push_back(ent);
        }

        // Balance delta: peg-in moves canonical → MWEB.
        // Canonical: lose all spent inputs minus canonical change (which
        // returns to canonical for R wallets).
        // MWEB: gain recipient + change_on_mweb (which is own change).
        const int64_t canonical_change = change_on_mweb ? int64_t(0) : op.ext_pegin_change;
        const int64_t mweb_change      = change_on_mweb ? op.ext_pegin_change : int64_t(0);
        UniValue balance_delta(UniValue::VOBJ);
        balance_delta.pushKV("canonical_sat",
                              -(op.ext_pegin_inputs_total) + canonical_change);
        balance_delta.pushKV("mweb_sat",
                              op.ext_pegin_amount + mweb_change);

        UniValue out(UniValue::VOBJ);
        out.pushKV("status", "estimated");
        out.pushKV("path", "peg-in");
        out.pushKV("txid", tx_id);
        out.pushKV("tx_hex", op.ext_pegin_tx_hex);
        out.pushKV("amount_sat",         op.ext_pegin_amount);
        out.pushKV("mweb_fee_sat",       op.ext_pegin_mweb_fee);
        out.pushKV("canonical_fee_sat",  op.ext_pegin_canonical_fee);
        out.pushKV("fee_sat",            op.ext_pegin_mweb_fee + op.ext_pegin_canonical_fee);
        out.pushKV("fee_rate_sat_per_vb", int64_t(rate));
        out.pushKV("mweb_change_sat",      change_on_mweb ? op.ext_pegin_change : int64_t(0));
        out.pushKV("canonical_change_sat", change_on_mweb ? int64_t(0) : op.ext_pegin_change);
        out.pushKV("change_on_mweb",       change_on_mweb);
        out.pushKV("inputs",               inputs_arr);
        out.pushKV("outputs",              outputs_arr);
        out.pushKV("balance_delta_sat",    balance_delta);
        out.pushKV("inputs_total_sat",   op.ext_pegin_inputs_total);
        out.pushKV("inputs_count",       int64_t(op.ext_pegin_inputs_outpoints.size()));
        op.result_json = out.write();
        op.phase = int32_t(ExtPegInPhase::Done);
        op.state = OYO_OP_DONE;
        return;
    }

    UniValue params(UniValue::VARR);
    params.push_back(op.ext_pegin_tx_hex);
    op.rpc_req = MakeRpc("oyo-ext-pegin", "sendrawtransaction", params);
    op.phase   = int32_t(ExtPegInPhase::WaitBroadcast);
    op.state   = OYO_OP_NEED_RPC;
}

void ExtPegInHandleResp(OyoOpImpl& op, const UniValue& r) {
    if (!r.isStr()) throw std::runtime_error("sendrawtransaction did not return a txid");
    const std::string txid = LowerHex(r.get_str());

    // Mark consumed canonical UTXOs as pending_out so balances update before
    // the next mempool diff. The next sync reaffirms or rolls back.
    OyoWalletImpl& w = *op.wallet;
    for (const auto& outpoint : op.ext_pegin_inputs_outpoints) {
        auto colon = outpoint.find(':');
        if (colon == std::string::npos) continue;
        UtxoKey key{outpoint.substr(0, colon),
                    static_cast<uint32_t>(std::stoul(outpoint.substr(colon + 1)))};
        for (const auto& b : w.bindings) {
            if (!b) continue;
            auto it = b->addr->utxos.find(key);
            if (it != b->addr->utxos.end() && !it->second.spent_pending) {
                Utxo& u = it->second;
                u.spent_pending = true;
                u.spent_pending_txid = txid;
                if (u.confirmed && !u.spent) {
                    b->addr->confirmed_sat   -= u.amount_sat;
                    b->addr->pending_out_sat += u.amount_sat;
                    NotifyWatchersLocked(*b->addr, -u.amount_sat);
                }
                break;
            }
        }
    }

    UniValue out(UniValue::VOBJ);
    out.pushKV("status", "sent");
    out.pushKV("txid",   txid);
    out.pushKV("amount_sat",        op.ext_pegin_amount);
    out.pushKV("mweb_fee_sat",      op.ext_pegin_mweb_fee);
    out.pushKV("canonical_fee_sat", op.ext_pegin_canonical_fee);
    // Change goes to MWEB index 0 (libmw CHANGE_INDEX convention) when the
    // wallet has its own MWEB side (privacy parity with node default for
    // peg-in); otherwise back to a fresh own-P2WPKH vout (R wallet).
    out.pushKV("mweb_change_sat",      op.ext_pegin_change_on_mweb ? op.ext_pegin_change : int64_t(0));
    out.pushKV("canonical_change_sat", op.ext_pegin_change_on_mweb ? int64_t(0) : op.ext_pegin_change);
    out.pushKV("change_on_mweb",       op.ext_pegin_change_on_mweb);
    out.pushKV("inputs_total_sat",  op.ext_pegin_inputs_total);
    out.pushKV("inputs_count",      int64_t(op.ext_pegin_inputs_outpoints.size()));
    op.result_json = out.write();
    op.phase = int32_t(ExtPegInPhase::Done);
    op.state = OYO_OP_DONE;
}

void DispatchExtPegIn(OyoOpImpl& op, const UniValue& env) {
    switch (ExtPegInPhase(op.phase)) {
        case ExtPegInPhase::WaitFeeRate: {
            try {
                op.fee_rate_sat_per_vb = ResolveFeeRateFromEstimateEnv(env);
            } catch (...) {
                op.fee_rate_sat_per_vb = kFallbackFeeRateSatPerVb;
            }
            FinalizeExtPegInOp(op);
            break;
        }
        case ExtPegInPhase::WaitBroadcast: {
            UniValue r = UnwrapRpcResult(env);
            ExtPegInHandleResp(op, r);
            break;
        }
        default: throw std::runtime_error("ext_pegin response in unexpected phase");
    }
}

}}  // namespace oyoltc::detail

// Bring all internal types/helpers into the global namespace for the
// rest of the TU (the `oyo_*` C-API impls and the two smaller anonymous
// blocks below). The detail namespace exists purely so mweb.h can
// forward-declare oyoltc::detail::MwebAddress.
using namespace oyoltc::detail;

// ---------------------------------------------------------------------------
// Public ABI
// ---------------------------------------------------------------------------

extern "C" {

OYO_API uint32_t oyo_version(void) { return OYOLTC_ABI_VERSION; }

OYO_API int32_t oyo_open(const char* workdir_utf8, OYO_CTX* out) {
    if (!out) return OYO_ERR_INVALID_ARG;
    try {
        G().acquire();
        auto ctx = std::make_unique<OyoCtxImpl>();
        if (workdir_utf8 && workdir_utf8[0] != '\0') {
            ctx->workdir = workdir_utf8;
            if (!EnsureDirExists(ctx->workdir)) {
                G().release();
                return OYO_ERR_INVALID_ARG;
            }
        }
        *out = reinterpret_cast<OYO_CTX>(ctx.release());
        return OYO_OK;
    } catch (...) { return OYO_ERR_INTERNAL; }
}

OYO_API int32_t oyo_close(OYO_CTX h) {
    auto* ctx = AsCtx(h);
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        for (auto& kv : ctx->ops) kv.second->magic = kMagicDead;
        ctx->ops.clear();
        for (auto& c : ctx->chains) {
            for (auto& w : c->wallets) w->magic = kMagicDead;
            c->wallets.clear();
            c->addresses.clear();
            c->magic = kMagicDead;
        }
        ctx->chains.clear();
        ctx->magic = kMagicDead;
    } catch (...) {}
    delete ctx;
    G().release();
    return OYO_OK;
}

OYO_API int32_t oyo_last_error(OYO_CTX h, int32_t* code, const uint8_t** msg, size_t* msg_len) {
    auto* ctx = AsCtx(h);
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    if (code) *code = ctx->last_err_code;
    if (msg) *msg = reinterpret_cast<const uint8_t*>(ctx->last_err_msg.data());
    if (msg_len) *msg_len = ctx->last_err_msg.size();
    return OYO_OK;
}

OYO_API int32_t oyo_chain_open(OYO_CTX h, const uint8_t* cfg, size_t cfg_len, OYO_CHAIN* out) {
    auto* ctx = AsCtx(h);
    if (!ctx || !out) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        UniValue c(UniValue::VOBJ);
        if (cfg && cfg_len) {
            c = ParseJson(cfg, cfg_len);
            if (!c.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "chain config not object");
        }
        auto chain = std::make_unique<OyoChainImpl>();
        chain->ctx = ctx;
        const UniValue& net = c["network"];
        if (net.isStr()) chain->network = NormalizeNetwork(net.get_str());
        chain->hrp = NetworkHrp(chain->network.empty() ? std::string("regtest") : chain->network);
        const UniValue& rb = c["rollback_window"];
        if (rb.isNum()) {
            int64_t v = rb.get_int64();
            if (v <= 0) v = kDefaultRollbackWin;
            if (size_t(v) > kMaxRollbackWin) v = kMaxRollbackWin;
            chain->rollback_window = size_t(v);
        }
        const UniValue& mrd = c["max_reorg_depth"];
        if (mrd.isNum()) {
            int64_t v = mrd.get_int64();
            if (v <= 0) return Fail(ctx, OYO_ERR_INVALID_ARG, "max_reorg_depth must be > 0");
            chain->max_reorg_depth = v;
        }
        const UniValue& tm = c["track_mempool"];
        if (tm.isBool()) chain->track_mempool = tm.get_bool();
        const UniValue& nbp = c["native_block_parse"];
        if (nbp.isBool()) chain->native_block_parse = nbp.get_bool();
        // SelectParams configures litecoin's address codec (DecodeDestination
        // / GetScriptForDestination) for the right network prefixes. Global
        // state — last chain context wins; we only run one chain in oyo-web.
        try {
            SelectParams(chain->network.empty() ? std::string("regtest") : chain->network);
        } catch (...) { /* already selected once is fine */ }

        // Open the per-chain LevelDB mirror. One DB owns meta/block/regular/MWEB
        // keys so progress, counters, and event rows commit atomically.
        std::string db_path;
        const std::string net_name = chain->network.empty() ? std::string("regtest") : chain->network;
        if (ctx->workdir.empty()) {
            db_path = ":memory:";
        } else {
            db_path = ctx->workdir + "/oyoltc-" + net_name + ".ldb";
        }
        chain->mweb_mirror = MwebMirror::Open(db_path);
        chain->regular_mirror = std::make_unique<RegularMirror>(chain->mweb_mirror.get());
        chain->block_trail    = std::make_unique<BlockTrail>(chain->mweb_mirror.get());
        int64_t mirror_state = chain->mweb_mirror->MetaGet(kMetaState, kMirrorBootstrap);
        // The legacy "indexing" state (a deferred index build inherited from the
        // SQLite backend) is a no-op for LevelDB — secondary keys are written
        // inline as the walk runs. Promote any on-disk indexing state straight to
        // working so old mirrors still load.
        if (mirror_state == kMirrorIndexing) {
            chain->mweb_mirror->MetaSet(kMetaState, kMirrorWorking);
            mirror_state = kMirrorWorking;
        } else if (mirror_state != kMirrorBootstrap && mirror_state != kMirrorWorking) {
            return Fail(ctx, OYO_ERR_INVALID_STATE, "unsupported mirror state in mirror meta");
        }
        chain->bootstrap_done = mirror_state == kMirrorWorking;
        // Bootstrap (initial genesis catch-up): commit in large batches for
        // throughput. Working (steady state): commit per block for promptness.
        chain->mirror_batch_target = chain->bootstrap_done ? 1 : kBootstrapBatchBlocks;
        // The MWEB mirror is built forward from blocks (every block's outputs
        // are appended) and self-seeds via the genesis walk, so it IS the seed —
        // no snapshot RPC needed on any network.
        // Resume the chain tip from the persistent block trail so a restart
        // continues the walk from disk (fast) instead of re-walking from genesis.
        // Empty trail -> tip stays -1 -> cold-start walks genesis. After restart,
        // a reorg below the resumed tip is handled by persistent block-trail
        // tail-delete; if no common ancestor remains, sync rebuilds from genesis.
        if (chain->block_trail) {
            int64_t t = chain->block_trail->Tip();
            if (t >= 0) {
                std::vector<uint8_t> hb = chain->block_trail->HashAt(t);
                if (hb.size() == 32) {
                    chain->tip_height = t;
                    chain->tip_hash   = ToHexLower(hb.data(), 32);
                }
            }
        }

        OYO_CHAIN p = reinterpret_cast<OYO_CHAIN>(chain.get());
        ctx->chains.push_back(std::move(chain));
        *out = p;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "chain_open failed"); }
}

OYO_API int32_t oyo_chain_close(OYO_CHAIN h) {
    auto* chain = AsChain(h);
    if (!chain) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        // Clean shutdown: commit any in-flight bootstrap batch and fsync it so a
        // graceful stop is fully durable (no re-walk on the next start, and the
        // mirror survives an immediate power loss after the stop). Best-effort —
        // a failure here must not block teardown.
        try { MirrorCommitBatchLocked(*chain, /*sync=*/true); } catch (...) {}
        for (auto it = ctx->ops.begin(); it != ctx->ops.end();) {
            if (it->second->chain == chain) { it->second->magic = kMagicDead; it = ctx->ops.erase(it); }
            else ++it;
        }
        for (auto& w : chain->wallets) w->magic = kMagicDead;
        chain->wallets.clear();
        chain->addresses.clear();
        chain->magic = kMagicDead;
        for (auto it = ctx->chains.begin(); it != ctx->chains.end(); ++it) {
            if (it->get() == chain) { ctx->chains.erase(it); break; }
        }
        return OYO_OK;
    } catch (...) { return OYO_ERR_INTERNAL; }
}

OYO_API int32_t oyo_chain_status(OYO_CHAIN h, const uint8_t** out_json, size_t* out_len) {
    auto* chain = AsChain(h);
    if (!chain || !out_json || !out_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        chain->status_cache = ChainStatusJsonLocked(*chain).write();
        *out_json = reinterpret_cast<const uint8_t*>(chain->status_cache.data());
        *out_len = chain->status_cache.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "chain_status failed"); }
}

// Classify a decoded canonical destination into the explorer's "kind" string.
// Empty for MWEB stealth / WitnessUnknown / CNoDestination (no canonical script).
static std::string CanonicalKind(const CTxDestination& dest) {
    if      (boost::get<PKHash>(&dest))              return "p2pkh";
    else if (boost::get<ScriptHash>(&dest))          return "p2sh";
    else if (boost::get<WitnessV0KeyHash>(&dest))    return "p2wpkh";
    else if (boost::get<WitnessV0ScriptHash>(&dest)) return "p2wsh";
    return std::string();
}

// MWEB arm of oyo_chain_address_status. A stealth address has no on-chain
// scriptPubKey index, so it cannot be resolved from the regular mirror; it is
// served from the chain's keychain-derived MwebAddress set (populated by the
// owner wallet's sync). Owned → balance + (with history) the full received[]
// journal (spent + unspent); unknown → foreign/opaque. Same shape the canonical
// arm emits, so the Go layer and explorer treat both uniformly.
static UniValue BuildMwebAddressStatusLocked(OyoChainImpl& c,
                                             const std::string& mweb_addr,
                                             bool history) {
    UniValue root(UniValue::VOBJ);
    root.pushKV("address", mweb_addr);
    root.pushKV("mweb",    true);
    root.pushKV("tip_height", c.tip_height);  // for relative-age rendering in the UI
    auto it = c.mweb_addresses.find(mweb_addr);
    if (it == c.mweb_addresses.end() || !it->second) {
        // Not one of ours — without the recipient's scan_secret the outputs are
        // undecryptable. Zeroed balance + foreign flag for the UI.
        root.pushKV("foreign",         true);
        root.pushKV("confirmed_sat",   int64_t(0));
        root.pushKV("pending_in_sat",  int64_t(0));
        root.pushKV("pending_out_sat", int64_t(0));
        return root;
    }
    const MwebAddress& ma = *it->second;
    root.pushKV("owned",           true);
    if (!ma.watchers.empty() && ma.watchers[0])
        root.pushKV("wallet", ma.watchers[0]->name);
    root.pushKV("confirmed_sat",   ma.confirmed_sat);
    root.pushKV("pending_in_sat",  ma.pending_in_sat);
    root.pushKV("pending_out_sat", ma.pending_out_sat);
    if (history) {
        UniValue received(UniValue::VARR);
        int64_t total_received = 0, total_spent = 0, recv_count = 0;
        bool recv_truncated = false;
        for (const auto& kv : ma.utxos) {
            const MwebUtxo& mu = kv.second;
            recv_count++;
            total_received += mu.amount_sat;
            if (mu.spent) total_spent += mu.amount_sat;
            if (received.size() < kMaxUtxosListed) {
                UniValue o(UniValue::VOBJ);
                o.pushKV("output_id",    HexStr(mu.output_id));
                o.pushKV("commitment",   HexStr(mu.commitment));
                o.pushKV("amount_sat",   mu.amount_sat);
                o.pushKV("height",       mu.height);
                o.pushKV("spent",        mu.spent);
                o.pushKV("spent_height", mu.spent_height);
                received.push_back(o);
            } else {
                recv_truncated = true;
            }
        }
        root.pushKV("total_received_sat", total_received);
        root.pushKV("total_spent_sat",    total_spent);
        root.pushKV("received_count",     recv_count);
        root.pushKV("received_truncated", recv_truncated);
        root.pushKV("received",           received);
    }
    return root;
}

OYO_API int32_t oyo_chain_address_status(OYO_CHAIN h, const uint8_t* req, size_t req_len,
                                         const uint8_t** out_json, size_t* out_len) {
    auto* chain = AsChain(h);
    if (!chain || !out_json || !out_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!req || !req_len) return Fail(ctx, OYO_ERR_INVALID_ARG, "request json required");
        if (!chain->regular_mirror) return Fail(ctx, OYO_ERR_INVALID_STATE, "regular mirror not open");
        UniValue rq = ParseJson(req, req_len);
        if (!rq.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "request not object");

        // Resolve the target from either an address (decoded against the chain's
        // network — SelectParams ran at open) or a raw script_hex. Canonical
        // addresses resolve to a scriptPubKey (served from the regular mirror);
        // MWEB stealth addresses have no canonical script and are dispatched to
        // the MWEB arm (BuildMwebAddressStatusLocked) below.
        const bool include_history = rq["history"].isBool() && rq["history"].get_bool();
        std::string canonical, kind, script_hex, requested;
        const UniValue& addr_v = rq["address"];
        const UniValue& shex_v = rq["script_hex"];
        if (addr_v.isStr() && !addr_v.get_str().empty()) {
            requested = addr_v.get_str();
            CTxDestination dest = DecodeDestination(requested);
            if (!IsValidDestination(dest))
                return Fail(ctx, OYO_ERR_INVALID_ARG, "invalid address");
            // MWEB stealth address: no canonical scriptPubKey. Dispatch to the
            // keychain-derived MWEB arm and return — one entrypoint, address-kind
            // dispatch (canonical → regular mirror; stealth → MwebAddress set).
            if (boost::get<StealthAddress>(&dest) != nullptr) {
                UniValue mroot = BuildMwebAddressStatusLocked(
                    *chain, EncodeDestination(dest), include_history);
                chain->address_status_cache = mroot.write();
                *out_json = reinterpret_cast<const uint8_t*>(chain->address_status_cache.data());
                *out_len  = chain->address_status_cache.size();
                return OYO_OK;
            }
            kind = CanonicalKind(dest);
            if (kind.empty())
                return Fail(ctx, OYO_ERR_INVALID_ARG, "address has no canonical script");
            CScript script = GetScriptForDestination(dest);
            script_hex = ToHexLower(script.data(), script.size());
            canonical  = EncodeDestination(dest);
        } else if (shex_v.isStr() && !shex_v.get_str().empty()) {
            script_hex = LowerHex(shex_v.get_str());
            std::vector<uint8_t> sb = HexToBytes(script_hex);
            CScript script(sb.data(), sb.data() + sb.size());
            CTxDestination dest;
            if (ExtractDestination(script, dest) && IsValidDestination(dest)) {
                canonical = EncodeDestination(dest);
                kind      = CanonicalKind(dest);
            }
        } else {
            return Fail(ctx, OYO_ERR_INVALID_ARG, "address or script_hex required");
        }

        std::vector<uint8_t> script_bytes = HexToBytes(script_hex);
        const int64_t tip = chain->tip_height;
        // EXPLORER bound: walk only the newest `scan_limit` outputs of this
        // script (newest-first), NOT the whole address. This serves an arbitrary
        // address, and a heavily-reused one (exchange / faucet / mining dest) can
        // have millions of outputs — an unbounded walk + per-output spent-Get ran
        // >20 min while holding the engine lock, hanging the whole API. Older
        // outputs are likelier already spent, so the newest window captures ~all
        // the live balance for normal addresses; `truncated` flags the rest as a
        // partial view, and `next_limit` (below) drives the UI's doubling "load
        // more". Default kAddrScanDefault; ?limit widens it with no ceiling.
        // Our OWN wallet addresses never reach here: their exact balance is the
        // in-memory watched-UTXO set, rebuilt by the full (uncapped) rescan walk.
        size_t scan_limit = kAddrScanDefault;
        if (rq["limit"].isNum()) {
            int64_t l = rq["limit"].get_int64();
            if (l > 0) scan_limit = size_t(l);   // no ceiling — operator decides depth
        }
        int64_t confirmed = 0, immature = 0, count = 0;
        int64_t total_received = 0, total_spent = 0, recv_count = 0;
        bool truncated = false;             // newest-N cap hit; more outputs exist
        UniValue utxos(UniValue::VARR);
        UniValue received(UniValue::VARR);  // populated only when include_history
        // Transient-watcher pending: build the mempool's spent-prevout index for
        // this request (every mempool input -> its spending txid), then derive
        // pending by membership against this address's UTXOs — no global index,
        // no outpoint->owner lookup. pending_out for confirmed coins is checked
        // in the walk below; pending_in (+ chained) is scanned after it.
        int64_t pending_in = 0, pending_out = 0;
        std::unordered_set<std::string> pending_txid_set;
        std::unordered_map<UtxoKey, std::string, UtxoKeyHash> mempool_spent;
        for (const auto& mkv : chain->mempool_view)
            for (const auto& in : mkv.second.ins)
                mempool_spent.emplace(UtxoKey{in.ptxid, in.pvout}, mkv.second.txid);
        // ONE newest-first pass over the window yields BOTH the unspent set
        // (spent_height == 0) and the received history — no second scan.
        truncated = chain->regular_mirror->ForEachByScript(script_bytes,
            [&](const std::vector<uint8_t>& outpoint, int64_t amount,
                int64_t height, int64_t spent_height, int64_t flags) {
                if (outpoint.size() != 36) return;
                const bool spent = spent_height > 0;
                const uint32_t vout = uint32_t(outpoint[32]) | (uint32_t(outpoint[33]) << 8) |
                                      (uint32_t(outpoint[34]) << 16) | (uint32_t(outpoint[35]) << 24);
                const bool is_cb  = (flags & RegularMirror::kFlagCoinbase) != 0;
                const bool is_pog = (flags & RegularMirror::kFlagPegOut)   != 0;

                // History side — every output in the window, spent or not.
                recv_count++;
                total_received += amount;
                if (spent) total_spent += amount;
                if (include_history && received.size() < kAddrListMax) {
                    UniValue o(UniValue::VOBJ);
                    o.pushKV("txid",         ToHexLower(outpoint.data(), 32));
                    o.pushKV("vout",         int64_t(vout));
                    o.pushKV("amount_sat",   amount);
                    o.pushKV("height",       height);
                    o.pushKV("spent",        spent);
                    o.pushKV("spent_height", spent_height);
                    o.pushKV("coinbase",     is_cb);
                    o.pushKV("pegout",       is_pog);
                    received.push_back(o);
                }
                if (spent) return;          // UTXO side — unspent only

                count++;
                confirmed += amount;
                // pending_out: this confirmed coin is being spent by a mempool tx.
                auto sit = mempool_spent.find(UtxoKey{ToHexLower(outpoint.data(), 32), vout});
                if (sit != mempool_spent.end()) {
                    pending_out += amount;
                    pending_txid_set.insert(sit->second);
                }
                // Same predicate as IsSpendableUtxoLocked: node accepts a spend
                // at nSpendHeight = tip+1, so maturity needs (tip - height) >=
                // (M - 1). Without a tip yet (cold start) treat as mature.
                bool mature = true;
                if (tip >= 0) {
                    if (is_cb  && (tip - height) < (kCoinbaseMaturity - 1)) mature = false;
                    if (is_pog && (tip - height) < (kPegoutMaturity   - 1)) mature = false;
                }
                if (!mature) immature += amount;
                if (utxos.size() < kAddrListMax) {
                    UniValue u(UniValue::VOBJ);
                    u.pushKV("txid",       ToHexLower(outpoint.data(), 32));
                    u.pushKV("vout",       int64_t(vout));
                    u.pushKV("amount_sat", amount);
                    u.pushKV("height",     height);
                    u.pushKV("coinbase",   is_cb);
                    u.pushKV("pegout",     is_pog);
                    u.pushKV("mature",     mature);
                    utxos.push_back(u);
                }
            }, scan_limit);

        // pending_in: unconfirmed receives to this script (+ chained pending_out:
        // a mempool output paying us that is itself spent by another mempool tx).
        // Works for ANY script with zero node queries; empty without track_mempool.
        for (const auto& mkv : chain->mempool_view) {
            const MempoolTxView& mtx = mkv.second;
            for (const auto& o : mtx.outs) {
                if (o.script_hex != script_hex) continue;
                pending_in += o.amount;
                pending_txid_set.insert(mtx.txid);
                auto sit = mempool_spent.find(UtxoKey{mtx.txid, o.vout});
                if (sit != mempool_spent.end()) {
                    pending_out += o.amount;
                    pending_txid_set.insert(sit->second);
                }
            }
        }
        UniValue pending_txids(UniValue::VARR);
        for (const auto& t : pending_txid_set) pending_txids.push_back(t);

        UniValue root(UniValue::VOBJ);
        root.pushKV("address",         canonical.empty() ? requested : canonical);
        root.pushKV("script_hex",      script_hex);
        root.pushKV("tip_height",      tip);  // lets the UI render output heights as relative age
        if (!kind.empty()) root.pushKV("kind", kind);
        root.pushKV("confirmed_sat",   confirmed);
        root.pushKV("immature_sat",    immature);
        root.pushKV("available_sat",   confirmed - immature);
        root.pushKV("pending_in_sat",  pending_in);
        root.pushKV("pending_out_sat", pending_out);
        root.pushKV("pending_txids",   pending_txids);
        root.pushKV("utxo_count",      count);
        root.pushKV("utxos_truncated", truncated);
        root.pushKV("scan_limit",      int64_t(scan_limit));
        // Next doubling step for the UI's "load more"; 0 = fully loaded (no more
        // outputs). No ceiling — the operator can keep doubling as deep as wanted.
        root.pushKV("next_limit",      int64_t(truncated ? scan_limit * 2 : 0));
        root.pushKV("utxos",           utxos);
        if (include_history) {
            root.pushKV("total_received_sat",  total_received);
            root.pushKV("total_spent_sat",     total_spent);
            root.pushKV("received_count",      recv_count);
            root.pushKV("received_truncated",  truncated);
            root.pushKV("received",            received);
        }

        chain->address_status_cache = root.write();
        *out_json = reinterpret_cast<const uint8_t*>(chain->address_status_cache.data());
        *out_len  = chain->address_status_cache.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "chain_address_status failed"); }
}

static int32_t open_regular_impl(OYO_CHAIN h,
                                        const uint8_t* wjson, size_t wlen,
                                        OYO_WALLET* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!wjson || !wlen) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet json required");
        UniValue c = ParseJson(wjson, wlen);
        if (!c.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "wallet config not object");

        auto w = std::make_unique<OyoWalletImpl>();
        w->chain = chain;
        const UniValue& nm = c["name"];
        if (!nm.isStr() || nm.get_str().empty()) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet name required");
        w->name = nm.get_str();
        for (const auto& ex : chain->wallets) if (ex->name == w->name) return Fail(ctx, OYO_ERR_EXISTS, "wallet exists");

        const UniValue& st = c["seed_type"];
        w->seed_type = st.isStr() ? st.get_str() : std::string("oyo_v1");
        if (w->seed_type != "oyo_v1") return Fail(ctx, OYO_ERR_UNSUPPORTED, "seed_type unsupported");
        const UniValue& sd = c["seed"];
        if (!sd.isStr()) return Fail(ctx, OYO_ERR_INVALID_ARG, "seed required");
        const std::string& s = sd.get_str();
        w->seed_bytes.assign(s.begin(), s.end());
        const UniValue& ac = c["address_count"];
        w->address_count = ac.isNum() ? int(ac.get_int64()) : 8;
        if (w->address_count <= 0) w->address_count = 8;
        // Cap is liberal — bindings live in a dense vector and grow
        // on demand via oyo_external_wallet_new_p2wpkh_address.
        if (w->address_count > 100000) return Fail(ctx, OYO_ERR_INVALID_ARG, "address_count too large");
        const UniValue& kd = c["address_kind"];
        w->address_kind = kd.isStr() ? kd.get_str() : std::string("p2wpkh");
        if (w->address_kind != "p2wpkh") return Fail(ctx, OYO_ERR_UNSUPPORTED, "only p2wpkh supported");

        w->wallet_kind = "node";
        w->has_p2wpkh  = true;
        w->has_mweb    = false;

        // Pre-allocate the requested initial address pool.
        const int initial_count = w->address_count;
        w->bindings.reserve(initial_count);
        w->address_count = 0;  // AllocateP2wpkhBindingLocked bumps this back up
        for (int i = 0; i < initial_count; ++i) {
            if (AllocateP2wpkhBindingLocked(*chain, *w) < 0) {
                return Fail(ctx, OYO_ERR_INTERNAL, "p2wpkh derivation failed");
            }
        }

        OYO_WALLET hp = reinterpret_cast<OYO_WALLET>(w.get());
        chain->wallets.push_back(std::move(w));
        *out = hp;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "wallet_open failed"); }
}

namespace {

bool ParseHex32(const std::string& s, std::array<uint8_t, 32>* out) {
    if (s.size() != 64) return false;
    auto bytes = ParseHex(s);
    if (bytes.size() != 32) return false;
    std::memcpy(out->data(), bytes.data(), 32);
    return true;
}

// Get-or-create a chain-level MwebAddress keyed by bech32 stealth address.
// Idempotent on the bech32 key; second wallet with the same seed allocating
// the same idx returns the existing address (and AttachMwebWatcherLocked is
// expected to be called by the caller).
MwebAddress* GetOrCreateMwebAddressLocked(OyoChainImpl& c,
                                          const std::string& mweb_address,
                                          uint32_t address_index,
                                          const std::array<uint8_t, 33>& scan_pubkey,
                                          const std::array<uint8_t, 33>& spend_pubkey) {
    auto it = c.mweb_addresses.find(mweb_address);
    if (it != c.mweb_addresses.end()) return it->second.get();
    auto a = std::make_unique<MwebAddress>();
    a->address_index = address_index;
    a->mweb_address  = mweb_address;
    a->scan_pubkey   = scan_pubkey;
    a->spend_pubkey  = spend_pubkey;
    auto* p = a.get();
    c.mweb_addresses.emplace(mweb_address, std::move(a));
    return p;
}

// Get-or-create a dedup'd chain-level MwebKeychain for (scan_secret, spend_pubkey).
// Two wallets opened on the same seed share a single keychain entry — their
// spend_pubkey_index is the same map, so per-output rewinds are O(addresses)
// rather than O(addresses × wallets).
MwebKeychain* GetOrCreateMwebKeychainLocked(OyoChainImpl& c,
    const std::array<uint8_t,32>& scan_secret,
    const std::array<uint8_t,33>& spend_pubkey) {
    for (auto& kc_ptr : c.mweb_keychains) {
        if (kc_ptr->scan_secret == scan_secret && kc_ptr->spend_pubkey == spend_pubkey) {
            return kc_ptr.get();
        }
    }
    auto kc = std::make_unique<MwebKeychain>();
    kc->scan_secret  = scan_secret;
    kc->spend_pubkey = spend_pubkey;
    auto* p = kc.get();
    c.mweb_keychains.push_back(std::move(kc));
    return p;
}

void AttachMwebWatcherLocked(MwebAddress& a, OyoWalletImpl* w) {
    for (OyoWalletImpl* x : a.watchers) if (x == w) return;
    a.watchers.push_back(w);
    w->mweb_balance_sat += a.confirmed_sat;
    w->balance_sat      += a.confirmed_sat;
    w->revision++;
}

void DetachMwebWatcherLocked(OyoChainImpl& c, MwebAddress& a, OyoWalletImpl* w) {
    auto it = std::find(a.watchers.begin(), a.watchers.end(), w);
    if (it == a.watchers.end()) return;
    a.watchers.erase(it);
    w->mweb_balance_sat -= a.confirmed_sat;
    w->balance_sat      -= a.confirmed_sat;
    w->revision++;
    if (a.watchers.empty()) {
        c.mweb_addresses.erase(a.mweb_address);
    }
}

// Allocates address slot at index `idx` for wallet `w`. The chain-level
// MwebAddress for this (seed, idx) is created once and re-used across
// wallets that share the seed; this wallet attaches as a watcher and gets
// a binding entry. idx must equal w.mweb_bindings.size() (append-only).
void AllocateMwebAddressLocked(OyoWalletImpl& w, uint32_t idx) {
    if (!w.mweb_keychain || !w.chain) return;
    SecretKey scan(w.mweb_keychain->scan_secret.data());
    // Re-derive spend secret from seed bytes only when we hold the seed.
    // Branch on seed_type: oyo_mweb_v1 / oyo_v1 hash the seed string with the
    // MWEB-domain tag; mweb_v0 stores scan||spend bytes directly in seed_bytes.
    SecretKey spend;
    if (w.seed_type == "oyo_mweb_v1" || w.seed_type == "oyo_v1") {
        auto master = MwebMasterFromSeedString(w.seed_bytes);
        scan  = oyoltc::mweb::DeriveScanSecret(master);
        spend = oyoltc::mweb::DeriveSpendSecret(master);
    } else {
        if (w.seed_bytes.size() != 64) return;
        spend = SecretKey(w.seed_bytes.data() + 32);
    }
    auto sa = oyoltc::mweb::DeriveStealthAddress(scan, spend, idx);

    std::string mweb_addr;
    try {
        StealthAddress sa_dest(sa.scan, sa.spend);
        mweb_addr = EncodeDestination(sa_dest);
    } catch (...) { mweb_addr.clear(); }
    if (mweb_addr.empty()) return;

    std::array<uint8_t, 33> scan_pub_bytes;
    std::array<uint8_t, 33> spend_pub_bytes;
    std::memcpy(scan_pub_bytes.data(),  sa.scan.data(),  33);
    std::memcpy(spend_pub_bytes.data(), sa.spend.data(), 33);

    MwebAddress* a = GetOrCreateMwebAddressLocked(*w.chain, mweb_addr, idx,
                                                  scan_pub_bytes, spend_pub_bytes);

    // Single map: B_i bytes → MwebAddress*. TryRewindOutput's match returns
    // the address pointer directly — no separate idx → addr indirection.
    w.mweb_keychain->spend_pubkey_index[a->spend_pubkey] = a;

    auto b = std::make_unique<MwebBinding>();
    b->addr  = a;
    b->index = idx;
    w.mweb_bindings.push_back(std::move(b));
    AttachMwebWatcherLocked(*a, &w);
}

}  // namespace

static int32_t open_mweb_impl(OYO_CHAIN h,
                                     const uint8_t* wjson, size_t wlen,
                                     OYO_WALLET* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!wjson || !wlen) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet json required");
        UniValue cfg = ParseJson(wjson, wlen);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "wallet config not object");

        auto w = std::make_unique<OyoWalletImpl>();
        w->chain = chain;
        const UniValue& nm = cfg["name"];
        if (!nm.isStr() || nm.get_str().empty())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet name required");
        w->name = nm.get_str();
        for (const auto& ex : chain->wallets)
            if (ex->name == w->name) return Fail(ctx, OYO_ERR_EXISTS, "wallet exists");
        // Optional birth height: skip the pre-birth prefix of the MWEB journal
        // during local bootstrap. 0 / absent = scan from genesis.
        const UniValue& bh = cfg["birth_height"];
        if (bh.isNum()) {
            int64_t v = bh.get_int64();
            if (v < 0) return Fail(ctx, OYO_ERR_INVALID_ARG, "birth_height must be >= 0");
            w->birth_height = v;
        }

        const UniValue& st = cfg["seed_type"];
        std::string seed_type = st.isStr() ? st.get_str() : std::string("oyo_mweb_v1");

        std::array<uint8_t, 32> scan_secret_bytes{};
        std::array<uint8_t, 33> spend_pubkey_bytes{};
        if (seed_type == "oyo_mweb_v1") {
            const UniValue& sd = cfg["seed"];
            if (!sd.isStr() || sd.get_str().empty())
                return Fail(ctx, OYO_ERR_INVALID_ARG, "seed required");
            // Free-form string (any length). Hash with the MWEB-domain
            // tag to get the 32-byte master, same chain Regular uses for
            // oyo_v1 — keeps the M/U dialog symmetric with R.
            const std::string& s = sd.get_str();
            std::vector<uint8_t> seed_string(s.begin(), s.end());
            auto master = MwebMasterFromSeedString(seed_string);
            SecretKey scan  = oyoltc::mweb::DeriveScanSecret(master);
            SecretKey spend = oyoltc::mweb::DeriveSpendSecret(master);
            std::memcpy(scan_secret_bytes.data(), scan.data(), 32);
            PublicKey spend_pub = PublicKey::From(spend);
            std::memcpy(spend_pubkey_bytes.data(), spend_pub.data(), 33);
            // Store the raw string in seed_bytes; AllocateMwebAddressLocked
            // re-derives the master each call (cheap; one SHA-256).
            w->seed_bytes = std::move(seed_string);
        } else if (seed_type == "mweb_v0") {
            const UniValue& ss = cfg["scan_secret"];
            const UniValue& ps = cfg["spend_secret"];
            if (!ss.isStr() || !ps.isStr())
                return Fail(ctx, OYO_ERR_INVALID_ARG, "scan_secret + spend_secret required");
            std::array<uint8_t, 32> scan_arr;
            std::array<uint8_t, 32> spend_arr;
            if (!ParseHex32(ss.get_str(), &scan_arr) || !ParseHex32(ps.get_str(), &spend_arr))
                return Fail(ctx, OYO_ERR_INVALID_ARG, "secrets must be 64-char hex");
            scan_secret_bytes = scan_arr;
            SecretKey spend(spend_arr.data());
            PublicKey spend_pub = PublicKey::From(spend);
            std::memcpy(spend_pubkey_bytes.data(), spend_pub.data(), 33);
            // seed_bytes layout: scan_secret (32) || spend_secret (32)
            w->seed_bytes.resize(64);
            std::memcpy(w->seed_bytes.data(),      scan_arr.data(),  32);
            std::memcpy(w->seed_bytes.data() + 32, spend_arr.data(), 32);
        } else {
            return Fail(ctx, OYO_ERR_UNSUPPORTED, "seed_type unsupported");
        }
        w->seed_type    = seed_type;
        w->wallet_kind  = "mweb";
        w->address_kind = "mweb";
        w->has_p2wpkh   = false;
        w->has_mweb     = true;

        // Dedup: re-use an existing keychain when another wallet already
        // opened on the same seed, so address allocations share state.
        w->mweb_keychain = GetOrCreateMwebKeychainLocked(
            *chain, scan_secret_bytes, spend_pubkey_bytes);

        // Bump revision so a fresh open is observable via StatusSince(0).
        w->revision = 1;

        OYO_WALLET hp = reinterpret_cast<OYO_WALLET>(w.get());
        chain->wallets.push_back(std::move(w));
        *out = hp;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "mweb_wallet_open failed"); }
}

// Universal external wallet: both P2WPKH (HD-derived bindings) and MWEB
// (master scan/spend) sides driven from the same 32-byte master seed via
// domain-separated SHA256 chains. `kinds` gates which sides are enabled
// — caller can opt into mweb-only ("kinds":["mweb"]) or p2wpkh-only
// ("kinds":["p2wpkh"]) for specialised use cases. Default is both.
//
// Wallet config JSON shape:
//   {"name":"...", "seed":"<hex64>", "kinds":["p2wpkh","mweb"],
//    "address_count": 20}
static int32_t open_universal_impl(OYO_CHAIN h,
                                         const uint8_t* wjson, size_t wlen,
                                         OYO_WALLET* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!wjson || !wlen) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet json required");
        UniValue cfg = ParseJson(wjson, wlen);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "wallet config not object");

        auto w = std::make_unique<OyoWalletImpl>();
        w->chain = chain;
        const UniValue& nm = cfg["name"];
        if (!nm.isStr() || nm.get_str().empty())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet name required");
        w->name = nm.get_str();
        for (const auto& ex : chain->wallets)
            if (ex->name == w->name) return Fail(ctx, OYO_ERR_EXISTS, "wallet exists");
        // Optional birth height: skip the pre-birth prefix of the MWEB journal
        // during local bootstrap. 0 / absent = scan from genesis.
        const UniValue& bh = cfg["birth_height"];
        if (bh.isNum()) {
            int64_t v = bh.get_int64();
            if (v < 0) return Fail(ctx, OYO_ERR_INVALID_ARG, "birth_height must be >= 0");
            w->birth_height = v;
        }

        const UniValue& sd = cfg["seed"];
        if (!sd.isStr() || sd.get_str().empty())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "seed required");
        // Free-form string. Canonical (P2WPKH) side derives via the
        // existing oyo_v1 chain (same as Regular wallets), MWEB side
        // hashes the string with the MWEB-domain tag to get its 32-byte
        // master. Symmetric: a Universal wallet with seed "alice"
        // produces the same canonical addresses as Regular("alice") and
        // the same MWEB addresses as MWEB("alice").
        const std::string& s = sd.get_str();
        std::vector<uint8_t> seed_string(s.begin(), s.end());

        // kinds defaults to ["p2wpkh","mweb"] if absent.
        bool want_p2wpkh = true;
        bool want_mweb   = true;
        if (cfg["kinds"].isArray()) {
            want_p2wpkh = false;
            want_mweb   = false;
            const UniValue& ks = cfg["kinds"];
            for (size_t i = 0; i < ks.size(); ++i) {
                if (!ks[i].isStr()) continue;
                const std::string s = ks[i].get_str();
                if      (s == "p2wpkh") want_p2wpkh = true;
                else if (s == "mweb")   want_mweb   = true;
                else return Fail(ctx, OYO_ERR_INVALID_ARG, "unknown kind: " + s);
            }
        }
        if (!want_p2wpkh && !want_mweb)
            return Fail(ctx, OYO_ERR_INVALID_ARG, "at least one kind required");

        const UniValue& ac = cfg["address_count"];
        int address_count = ac.isNum() ? int(ac.get_int64()) : 8;
        if (address_count <= 0) address_count = 8;
        // Liberal cap — bindings grow on demand.
        if (address_count > 100000) return Fail(ctx, OYO_ERR_INVALID_ARG, "address_count too large");

        w->seed_type    = "oyo_v1";
        w->seed_bytes   = seed_string;       // raw user string (variable length)
        w->address_kind = "p2wpkh";          // even when only mweb side is enabled
        w->address_count = 0;                // AllocateP2wpkhBindingLocked bumps this
        w->has_p2wpkh   = want_p2wpkh;
        w->has_mweb     = want_mweb;
        w->wallet_kind  = (want_p2wpkh && want_mweb) ? "universal"
                        : (want_mweb ? "mweb" : "node");

        if (want_p2wpkh) {
            w->bindings.reserve(address_count);
            for (int i = 0; i < address_count; ++i) {
                if (AllocateP2wpkhBindingLocked(*chain, *w) < 0) {
                    return Fail(ctx, OYO_ERR_INTERNAL, "p2wpkh derivation failed");
                }
            }
        }

        if (want_mweb) {
            // Hash the user-supplied string with the MWEB-domain tag to
            // get the 32-byte master, then run the existing scan/spend
            // derivation. Same chain mweb-only wallets use, so a Universal
            // wallet with seed S has the same MWEB addresses as MWEB(S).
            auto master = MwebMasterFromSeedString(seed_string);
            SecretKey scan  = oyoltc::mweb::DeriveScanSecret(master);
            SecretKey spend = oyoltc::mweb::DeriveSpendSecret(master);
            std::array<uint8_t, 32> scan_bytes{};
            std::array<uint8_t, 33> spend_pubkey_bytes{};
            std::memcpy(scan_bytes.data(), scan.data(), 32);
            PublicKey spend_pub = PublicKey::From(spend);
            std::memcpy(spend_pubkey_bytes.data(), spend_pub.data(), 33);

            // Dedup: shared keychain when same seed is already open.
            // MWEB addresses themselves are pre-allocated by the Go layer
            // via NewAddress(AddrMweb) so wallet.mwebAddressCount tracks
            // them for re-load. Index 0 (CHANGE_INDEX convention) is the
            // first allocated by that loop.
            w->mweb_keychain = GetOrCreateMwebKeychainLocked(
                *chain, scan_bytes, spend_pubkey_bytes);
        }

        // Fresh open: revision starts at 1 so StatusSince(0) is observable
        // even when no bindings have been attached yet (edge case for
        // mweb-only kind without any addresses pre-allocated).
        if (w->revision == 0) w->revision = 1;

        OYO_WALLET hp = reinterpret_cast<OYO_WALLET>(w.get());
        chain->wallets.push_back(std::move(w));
        *out = hp;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "external_wallet_open failed"); }
}

// Allocate the next canonical address of the given kind ("p2wpkh" |
// "p2pkh" | "p2sh-p2wpkh"). All three share the oyo_v1 keychain (P2.1).
static int32_t new_canonical_address_impl(OYO_WALLET wh, const std::string& kind,
                                          int32_t* out_index,
                                          const uint8_t** out_addr,
                                          size_t* out_addr_len) {
    auto* w = AsWallet(wh);
    if (!w || !out_index || !out_addr || !out_addr_len) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!w->has_p2wpkh)
            return Fail(ctx, OYO_ERR_INVALID_STATE, "wallet has no canonical side");
        if (w->watch_only)
            return Fail(ctx, OYO_ERR_INVALID_STATE, "watch-only wallets use add_address instead");
        int idx = AllocateCanonicalBindingLocked(*chain, *w, kind, /*runtime_alloc=*/true);
        if (idx < 0) return Fail(ctx, OYO_ERR_INTERNAL, "binding allocation failed");
        const std::string& addr = w->bindings[idx]->addr->address;
        *out_index    = static_cast<int32_t>(idx);
        *out_addr     = reinterpret_cast<const uint8_t*>(addr.data());
        *out_addr_len = addr.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "new_canonical_address failed"); }
}

static int32_t new_mweb_address_impl(OYO_WALLET wh,
                                            int32_t* out_index,
                                            const uint8_t** out_addr,
                                            size_t* out_addr_len) {
    auto* w = AsWallet(wh);
    if (!w || !out_index || !out_addr || !out_addr_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain ? w->chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!w->has_mweb || !w->mweb_keychain)
            return Fail(ctx, OYO_ERR_INVALID_STATE, "wallet has no mweb side");
        uint32_t idx = static_cast<uint32_t>(w->mweb_bindings.size());
        AllocateMwebAddressLocked(*w, idx);
        if (idx >= w->mweb_bindings.size() ||
            !w->mweb_bindings[idx] || !w->mweb_bindings[idx]->addr)
            return Fail(ctx, OYO_ERR_INTERNAL, "address allocation failed");
        const std::string& addr = w->mweb_bindings[idx]->addr->mweb_address;
        if (addr.empty())
            return Fail(ctx, OYO_ERR_INTERNAL, "stealth address encode failed");
        *out_index    = static_cast<int32_t>(idx);
        *out_addr     = reinterpret_cast<const uint8_t*>(addr.data());
        *out_addr_len = addr.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "mweb_wallet_new_address failed"); }
}

static int32_t open_watch_impl(OYO_CHAIN h,
                                      const uint8_t* wjson, size_t wlen,
                                      OYO_WALLET* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!wjson || !wlen) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet json required");
        UniValue cfg = ParseJson(wjson, wlen);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "wallet config not object");

        auto w = std::make_unique<OyoWalletImpl>();
        w->chain = chain;
        const UniValue& nm = cfg["name"];
        if (!nm.isStr() || nm.get_str().empty()) return Fail(ctx, OYO_ERR_INVALID_ARG, "wallet name required");
        w->name = nm.get_str();
        for (const auto& ex : chain->wallets) if (ex->name == w->name) return Fail(ctx, OYO_ERR_EXISTS, "wallet exists");
        w->seed_type    = "watch";
        w->wallet_kind  = "watch";
        w->address_kind = "mixed";
        w->watch_only   = true;
        // Bump revision once so a fresh open is observable via StatusSince(0)
        // — for derivation wallets the initial AttachWatcher calls already
        // moved revision past 0; watch wallets start with no bindings.
        w->revision = 1;
        OYO_WALLET hp = reinterpret_cast<OYO_WALLET>(w.get());
        chain->wallets.push_back(std::move(w));
        *out = hp;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "watch_wallet_open failed"); }
}

namespace {

// Returns kind string and script hex for a decoded address. address_str is
// the canonical form (re-encoded by GetDestinationAddress for consistency).
// On unsupported destination (StealthAddress / WitnessUnknown / invalid),
// kind comes back empty.
struct DecodedAddress {
    std::string kind;
    std::string script_hex;
    std::string canonical;
};

DecodedAddress DecodeAddressForWatch(const std::string& addr_str) {
    DecodedAddress out;
    CTxDestination dest = DecodeDestination(addr_str);
    if (!IsValidDestination(dest)) return out;
    if (boost::get<PKHash>(&dest))             out.kind = "p2pkh";
    else if (boost::get<ScriptHash>(&dest))    out.kind = "p2sh";
    else if (boost::get<WitnessV0KeyHash>(&dest))    out.kind = "p2wpkh";
    else if (boost::get<WitnessV0ScriptHash>(&dest)) out.kind = "p2wsh";
    else return out; // StealthAddress / WitnessUnknown — unsupported
    CScript script = GetScriptForDestination(dest);
    out.script_hex = ToHexLower(script.data(), script.size());
    out.canonical  = EncodeDestination(dest);
    return out;
}

} // namespace

static int32_t watch_add_address_impl(OYO_WALLET wh,
                                             const uint8_t* address, size_t alen,
                                             int32_t* out_binding_index) {
    auto* w = AsWallet(wh);
    if (!w || !address || alen == 0 || !out_binding_index) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain;
    auto* ctx   = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!w->watch_only) return Fail(ctx, OYO_ERR_INVALID_STATE, "not a watch-only wallet");
        std::string addr_str(reinterpret_cast<const char*>(address), alen);
        DecodedAddress dec = DecodeAddressForWatch(addr_str);
        if (dec.kind.empty()) return Fail(ctx, OYO_ERR_UNSUPPORTED, "address invalid or unsupported kind");
        // Reject duplicate within this wallet (different slots watching the
        // same script would double-count balances on the watcher side).
        for (const auto& b : w->bindings) {
            if (b && b->addr->script_hex == dec.script_hex)
                return Fail(ctx, OYO_ERR_EXISTS, "address already attached to this wallet");
        }
        bool was_new = !chain->addresses.count(dec.script_hex);
        Address* addr = GetOrCreateAddressLocked(*chain, dec.script_hex, dec.kind, dec.canonical);
        auto b = std::make_unique<Binding>();
        b->addr  = addr;
        b->index = int(w->bindings.size());
        int idx = b->index;
        w->bindings.push_back(std::move(b));
        AttachWatcherLocked(*addr, w);
        if (was_new) {
            ReplayMempoolForAddressLocked(*chain, *addr);
        }
        *out_binding_index = idx;
        ctx->last_err_code = 0;
        ctx->last_err_msg.clear();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "watch_wallet_add_address failed"); }
}

static int32_t watch_remove_address_impl(OYO_WALLET wh, int32_t idx) {
    auto* w = AsWallet(wh);
    if (!w) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain;
    auto* ctx   = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!w->watch_only) return Fail(ctx, OYO_ERR_INVALID_STATE, "not a watch-only wallet");
        if (idx < 0 || size_t(idx) >= w->bindings.size() || !w->bindings[idx])
            return Fail(ctx, OYO_ERR_INVALID_ARG, "binding_index out of range or removed");
        Address* a = w->bindings[idx]->addr;
        DetachWatcherLocked(*chain, *a, w);
        w->bindings[idx].reset(); // null slot, index stays stable
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "watch_wallet_remove_address failed"); }
}

OYO_API int32_t oyo_wallet_close(OYO_WALLET h) {
    auto* w = AsWallet(h);
    if (!w) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain;
    auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        for (auto it = ctx->ops.begin(); it != ctx->ops.end();) {
            if (it->second->wallet == w) { it->second->magic = kMagicDead; it = ctx->ops.erase(it); }
            else ++it;
        }
        for (auto& b : w->bindings) {
            if (b) DetachWatcherLocked(*chain, *b->addr, w);
        }
        w->bindings.clear();
        // Detach from chain-level MWEB addresses. When this wallet was the
        // last watcher of an address, the address (and any chain state tied
        // to it — utxo index entries, mempool entries, keychain index slots)
        // are dropped before the address itself is erased.
        for (auto& b : w->mweb_bindings) {
            if (!b || !b->addr) continue;
            MwebAddress* a = b->addr;
            const bool last_watcher = (a->watchers.size() == 1);
            if (last_watcher) {
                for (auto it = chain->mweb_utxo_index.begin();
                     it != chain->mweb_utxo_index.end();) {
                    if (it->second.owner_addr == a) {
                        chain->mweb_mempool_pending_out.erase(it->first);
                        it = chain->mweb_utxo_index.erase(it);
                    } else { ++it; }
                }
                for (auto it = chain->mweb_mempool_pending.begin();
                     it != chain->mweb_mempool_pending.end();) {
                    if (it->second.owner_addr == a)
                        it = chain->mweb_mempool_pending.erase(it);
                    else ++it;
                }
                if (w->mweb_keychain) {
                    w->mweb_keychain->spend_pubkey_index.erase(a->spend_pubkey);
                }
            }
            DetachMwebWatcherLocked(*chain, *a, w);
        }
        w->mweb_bindings.clear();
        // Drop the keychain entry only when no other wallet still references
        // it. Two wallets opened on the same seed share a keychain (dedup'd
        // by GetOrCreateMwebKeychainLocked); closing one mustn't dangle the
        // other's pointer.
        if (w->mweb_keychain) {
            MwebKeychain* kc = w->mweb_keychain;
            bool used_elsewhere = false;
            for (auto& other : chain->wallets) {
                if (other.get() != w && other->mweb_keychain == kc) {
                    used_elsewhere = true;
                    break;
                }
            }
            if (!used_elsewhere) {
                for (auto it = chain->mweb_keychains.begin();
                     it != chain->mweb_keychains.end(); ++it) {
                    if (it->get() == kc) { chain->mweb_keychains.erase(it); break; }
                }
            }
            w->mweb_keychain = nullptr;
        }
        w->magic = kMagicDead;
        for (auto it = chain->wallets.begin(); it != chain->wallets.end(); ++it) {
            if (it->get() == w) { chain->wallets.erase(it); break; }
        }
        return OYO_OK;
    } catch (...) { return OYO_ERR_INTERNAL; }
}

OYO_API int32_t oyo_wallet_revision(OYO_WALLET h, uint64_t* out_rev) {
    auto* w = AsWallet(h);
    if (!w || !out_rev) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain ? w->chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    *out_rev = w->revision;
    return OYO_OK;
}

OYO_API int32_t oyo_wallet_status_since(OYO_WALLET h,
                                        uint64_t since_rev,
                                        uint64_t* out_rev,
                                        const uint8_t** out_json,
                                        size_t* out_len) {
    auto* w = AsWallet(h);
    if (!w || !out_rev) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain ? w->chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        const int64_t cur_tip = w->chain ? w->chain->tip_height : -1;
        // No-change path skipped when the chain tip moved past the cache —
        // immature_sat is a function of (tip - utxo.height) and would
        // otherwise stay stale. revision alone doesn't capture it because
        // a chain.Sync that advances tip past N+5 doesn't touch any
        // address-level state of this wallet.
        if (since_rev == w->revision && cur_tip == w->status_cache_tip) {
            *out_rev = w->revision;
            return OYO_NO_CHANGE;
        }
        if (!out_json || !out_len) return OYO_ERR_INVALID_ARG;
        if (w->status_cache.empty() ||
            w->status_cache_rev != w->revision ||
            w->status_cache_tip != cur_tip) {
            w->status_cache     = WalletStatusJsonLocked(*w).write();
            w->status_cache_rev = w->revision;
            w->status_cache_tip = cur_tip;
        }
        *out_rev = w->revision;
        *out_json = reinterpret_cast<const uint8_t*>(w->status_cache.data());
        *out_len = w->status_cache.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "wallet_status_since failed"); }
}

// Export the wallet's secret material (seed-derived keys) for backup /
// audit. DANGEROUS — exposes full spend authority; the caller gates access.
// Watch-only wallets have nothing to export (addresses are imported).
OYO_API int32_t oyo_wallet_export_secrets(OYO_WALLET h,
                                          const uint8_t** out_json,
                                          size_t* out_len) {
    auto* w = AsWallet(h);
    if (!w || !out_json || !out_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain ? w->chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (w->watch_only) {
            return Fail(ctx, OYO_ERR_UNSUPPORTED,
                        "watch-only wallet has no secrets to export");
        }
        const std::string net = w->chain->network.empty()
            ? std::string("regtest") : w->chain->network;
        UniValue out(UniValue::VOBJ);
        out.pushKV("seed_type", w->seed_type);

        // Canonical (P2WPKH) side: a WIF per live binding. Same oyo_v1 chain
        // the addresses were derived from, so these import 1:1 into any
        // standard wallet (e.g. litecoind importprivkey).
        if (w->has_p2wpkh) {
            UniValue arr(UniValue::VARR);
            for (const auto& b : w->bindings) {
                if (!b || !b->addr) continue;
                CKey key;
                if (!DeriveOyoV1Key(w->seed_bytes, net, b->index, key)) continue;
                UniValue e(UniValue::VOBJ);
                e.pushKV("index", b->index);
                e.pushKV("address", b->addr->address);
                e.pushKV("wif", EncodeSecret(key));
                arr.push_back(e);
            }
            out.pushKV("p2wpkh", arr);
        }

        // MWEB side: the master scan + spend secrets. oyo_mweb_v1 / oyo_v1
        // re-derive them from the seed string (the scan secret is also kept
        // in the keychain, but spend is private — re-derive for export);
        // mweb_v0 stored scan(32)||spend(32) raw in seed_bytes.
        if (w->has_mweb) {
            UniValue m(UniValue::VOBJ);
            if (w->seed_type == "oyo_mweb_v1" || w->seed_type == "oyo_v1") {
                auto master = MwebMasterFromSeedString(w->seed_bytes);
                auto scan   = oyoltc::mweb::DeriveScanSecret(master);
                auto spend  = oyoltc::mweb::DeriveSpendSecret(master);
                m.pushKV("scan_secret",  ToHexLower(scan.data(), 32));
                m.pushKV("spend_secret", ToHexLower(spend.data(), 32));
            } else if (w->seed_bytes.size() == 64) {   // mweb_v0
                m.pushKV("scan_secret",  ToHexLower(w->seed_bytes.data(), 32));
                m.pushKV("spend_secret", ToHexLower(w->seed_bytes.data() + 32, 32));
            }
            UniValue addrs(UniValue::VARR);
            for (const auto& mb : w->mweb_bindings) {
                if (!mb || !mb->addr) continue;
                UniValue e(UniValue::VOBJ);
                e.pushKV("index", int64_t(mb->index));
                e.pushKV("address", mb->addr->mweb_address);
                addrs.push_back(e);
            }
            m.pushKV("addresses", addrs);
            out.pushKV("mweb", m);
        }

        w->secrets_cache = out.write();
        *out_json = reinterpret_cast<const uint8_t*>(w->secrets_cache.data());
        *out_len  = w->secrets_cache.size();
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "wallet_export_secrets failed"); }
}

OYO_API int32_t oyo_chain_sync(OYO_CHAIN h, OYO_OP* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->kind = OpKind::Sync;
        StartSyncOp(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "chain_sync failed"); }
}

OYO_API int32_t oyo_mempool_sync(OYO_CHAIN h, OYO_OP* out) {
    auto* chain = AsChain(h);
    if (!chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->kind = OpKind::MempoolSync;
        StartMempoolSyncOp(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "mempool_sync failed"); }
}

// Parses cfg.outputs[] into op.send_outputs when the caller passes
// multi-recipient. Validates entries, requires same address class
// across all (mixed canonical + stealth in one tx isn't supported by
// the underlying finalizers — that's the mixed-kind feature still on
// the roadmap). Leaves op.send_outputs empty when cfg has no outputs[]
// — the impl_send entry then falls back to legacy single-recipient
// (cfg.to + cfg.amount_sat). send_all is incompatible with outputs[]
// (drain semantics are ambiguous across N recipients).
static int32_t ParseSendOutputsLocked(OyoOpImpl& op, OyoCtxImpl* ctx,
                                       const UniValue& cfg) {
    const UniValue& outs = cfg["outputs"];
    if (!outs.isArray()) return OYO_OK;
    const bool send_all = cfg["send_all"].isBool() && cfg["send_all"].get_bool();
    if (send_all) return Fail(ctx, OYO_ERR_INVALID_ARG,
                              "outputs[] incompatible with send_all");
    if (outs.size() == 0) return Fail(ctx, OYO_ERR_INVALID_ARG,
                                       "outputs[] must be non-empty");
    bool first_is_stealth = false;
    bool drain_seen = false;
    for (size_t i = 0; i < outs.size(); ++i) {
        const UniValue& o = outs[i];
        if (!o.isObject())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "outputs[i] must be object");
        const UniValue& a = o["address"];
        const UniValue& v = o["amount_sat"];
        const UniValue& mx = o["max"];
        const bool is_drain = mx.isBool() && mx.get_bool();   // P2.4
        if (!a.isStr())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "outputs[i] requires address");
        int64_t amount = 0;
        if (is_drain) {
            // The drain output's amount is computed (remainder); amount_sat
            // is ignored. Exactly one drain output is allowed.
            if (drain_seen)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "only one output may set max:true");
            drain_seen = true;
        } else {
            if (!v.isNum())
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "outputs[i] requires amount_sat (or max:true)");
            amount = v.get_int64();
            if (amount <= 0)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "outputs[i].amount_sat must be > 0");
        }
        const std::string addr = a.get_str();
        CTxDestination dest = DecodeDestination(addr);
        if (!IsValidDestination(dest))
            return Fail(ctx, OYO_ERR_INVALID_ARG,
                        "outputs[i] invalid address: " + addr);
        const bool is_stealth = boost::get<StealthAddress>(&dest) != nullptr;
        if (i == 0) first_is_stealth = is_stealth;
        else if (is_stealth != first_is_stealth)
            return Fail(ctx, OYO_ERR_UNSUPPORTED,
                        "mixed canonical + stealth outputs in one tx not supported");
        op.send_outputs.push_back({addr, amount, is_drain});
    }
    return OYO_OK;
}

// Parses cfg.inputs[] into op.send_inputs. The expected shape depends
// on the path: canonical paths (regular_send, pegin_send) expect
// {"txid": "<hex>", "vout": N}; the MWEB path expects {"commitment":
// "<hex>"}. `expect_mweb` selects the validation flavor. Empty / absent
// inputs[] is a valid no-op (auto-select). Each entry must be unique
// within the array. The match against actual wallet UTXOs (existence,
// confirmed-not-spent, kind) happens later in the finalizer where we
// already iterate the wallet's UTXO maps.
static int32_t ParseSendInputsLocked(OyoOpImpl& op, OyoCtxImpl* ctx,
                                      const UniValue& cfg, bool expect_mweb) {
    const UniValue& ins = cfg["inputs"];
    if (!ins.isArray()) return OYO_OK;
    if (ins.size() == 0) return OYO_OK;
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < ins.size(); ++i) {
        const UniValue& e = ins[i];
        if (!e.isObject())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "inputs[i] must be object");
        OyoOpImpl::InputSpec spec;
        if (expect_mweb) {
            const UniValue& c = e["commitment"];
            if (!c.isStr())
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "inputs[i] requires commitment (mweb path)");
            spec.commitment = LowerHex(c.get_str());
            if (spec.commitment.empty())
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "inputs[i].commitment is empty");
            if (!seen.insert(spec.commitment).second)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "duplicate input: " + spec.commitment);
        } else {
            const UniValue& t = e["txid"];
            const UniValue& v = e["vout"];
            if (!t.isStr() || !v.isNum())
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "inputs[i] requires txid + vout (canonical path)");
            spec.txid = LowerHex(t.get_str());
            int64_t vn = v.get_int64();
            if (vn < 0 || vn > UINT32_MAX)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "inputs[i].vout out of range");
            spec.vout = static_cast<uint32_t>(vn);
            std::string key = spec.txid + ":" + std::to_string(spec.vout);
            if (!seen.insert(key).second)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "duplicate input: " + key);
        }
        op.send_inputs.push_back(std::move(spec));
    }
    return OYO_OK;
}

static int32_t regular_send_impl(OYO_WALLET h,
                                        const uint8_t* json, size_t json_len,
                                        OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out || !json || !json_len) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        UniValue cfg = ParseJson(json, json_len);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "send config not object");
        const bool has_outputs = cfg["outputs"].isArray();
        const UniValue& to  = cfg["to"];
        const UniValue& amt = cfg["amount_sat"];
        const UniValue& sa  = cfg["send_all"];
        const bool send_all = sa.isBool() && sa.get_bool();
        if (!has_outputs) {
            if (!to.isStr()) return Fail(ctx, OYO_ERR_INVALID_ARG, "to required");
            if (!send_all && !amt.isNum()) {
                return Fail(ctx, OYO_ERR_INVALID_ARG, "amount_sat required (or send_all=true)");
            }
        }

        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w; op->kind = OpKind::RegularSend;
        op->send_all = send_all;
        op->dry_run  = cfg["dry_run"].isBool() && cfg["dry_run"].get_bool();
        int32_t rc = ParseSendOutputsLocked(*op, ctx, cfg);
        if (rc != OYO_OK) return rc;
        if (op->send_outputs.empty()) {
            op->reg_send_to     = to.get_str();
            op->reg_send_amount = send_all ? 0 : amt.get_int64();
        } else {
            // Multi-recipient — synthesize legacy single-recipient fields
            // from the first entry so log lines and result_json's existing
            // fields (amount_sat, recipient label) stay populated.
            op->reg_send_to     = op->send_outputs[0].address;
            op->reg_send_amount = op->send_outputs[0].amount_sat;
        }
        if (cfg["fee_rate_sat_per_vb"].isNum()) {
            int64_t r = cfg["fee_rate_sat_per_vb"].get_int64();
            if (r < 1) return Fail(ctx, OYO_ERR_INVALID_ARG, "fee_rate_sat_per_vb must be >= 1");
            op->fee_rate_sat_per_vb = static_cast<uint64_t>(r);
        }
        rc = ParseSendInputsLocked(*op, ctx, cfg, /*expect_mweb=*/false);
        if (rc != OYO_OK) return rc;
        // Custom change address (P2.3) — must be a valid canonical
        // destination (a fat-finger / invalid value would otherwise burn
        // the change, so reject up front rather than build a bad tx).
        if (cfg["change_address"].isStr() && !cfg["change_address"].get_str().empty()) {
            const std::string ca = cfg["change_address"].get_str();
            CTxDestination cd = DecodeDestination(ca);
            if (!IsValidDestination(cd))
                return Fail(ctx, OYO_ERR_INVALID_ARG, "invalid change_address: " + ca);
            if (boost::get<StealthAddress>(&cd) != nullptr)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "change_address must be canonical, not MWEB stealth");
            op->change_address = ca;
        }
        StartRegularSendOp(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_ARG, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "regular_wallet_send failed"); }
}

static int32_t pegin_send_impl(OYO_WALLET h,
                                               const uint8_t* json, size_t json_len,
                                               OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out || !json || !json_len) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        UniValue cfg = ParseJson(json, json_len);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "send config not object");
        const bool has_outputs = cfg["outputs"].isArray();
        const UniValue& to  = cfg["to"];
        const UniValue& amt = cfg["amount_sat"];
        const UniValue& sa  = cfg["send_all"];
        const bool send_all = sa.isBool() && sa.get_bool();
        if (!has_outputs) {
            if (!to.isStr()) return Fail(ctx, OYO_ERR_INVALID_ARG, "to required");
            if (!send_all && !amt.isNum()) {
                return Fail(ctx, OYO_ERR_INVALID_ARG, "amount_sat required (or send_all=true)");
            }
        }

        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w; op->kind = OpKind::ExtPegIn;
        op->send_all             = send_all;
        op->dry_run              = cfg["dry_run"].isBool() && cfg["dry_run"].get_bool();
        int32_t rc = ParseSendOutputsLocked(*op, ctx, cfg);
        if (rc != OYO_OK) return rc;
        // Custom change (P2.3) and drain output (P2.4) are canonical-send
        // only; the peg-in builder owns its own change placement.
        for (const auto& o : op->send_outputs)
            if (o.drain) return Fail(ctx, OYO_ERR_UNSUPPORTED,
                                     "max (drain) output not supported for peg-in (R→M)");
        if (cfg["change_address"].isStr() && !cfg["change_address"].get_str().empty())
            return Fail(ctx, OYO_ERR_UNSUPPORTED,
                        "change_address not supported for peg-in (R→M)");
        if (op->send_outputs.empty()) {
            op->ext_pegin_to     = to.get_str();
            op->ext_pegin_amount = send_all ? 0 : amt.get_int64();
        } else {
            // libmw's BuildPegInMwebPart accepts a single stealth recipient
            // (+ optional own-side change). Multi-recipient peg-in would
            // need a refactor of the helper — reject upfront with a clear
            // error so the frontend can keep one input row for peg-ins.
            if (op->send_outputs.size() > 1) {
                return Fail(ctx, OYO_ERR_UNSUPPORTED,
                            "multi-recipient peg-in (R→M) not supported yet");
            }
            op->ext_pegin_to     = op->send_outputs[0].address;
            op->ext_pegin_amount = op->send_outputs[0].amount_sat;
        }
        if (cfg["fee_rate_sat_per_vb"].isNum()) {
            int64_t r = cfg["fee_rate_sat_per_vb"].get_int64();
            if (r < 1) return Fail(ctx, OYO_ERR_INVALID_ARG, "fee_rate_sat_per_vb must be >= 1");
            op->fee_rate_sat_per_vb = static_cast<uint64_t>(r);
        }
        rc = ParseSendInputsLocked(*op, ctx, cfg, /*expect_mweb=*/false);
        if (rc != OYO_OK) return rc;
        StartExtPegInOp(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_ARG, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "external_wallet_pegin_send failed"); }
}

static int32_t mweb_send_impl(OYO_WALLET h,
                                     const uint8_t* json, size_t json_len,
                                     OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out || !json || !json_len) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        UniValue cfg = ParseJson(json, json_len);
        if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "send config not object");
        const bool has_outputs = cfg["outputs"].isArray();
        const UniValue& to  = cfg["to"];
        const UniValue& amt = cfg["amount_sat"];
        const UniValue& sa  = cfg["send_all"];
        const bool send_all = sa.isBool() && sa.get_bool();
        if (!has_outputs) {
            if (!to.isStr()) return Fail(ctx, OYO_ERR_INVALID_ARG, "to required");
            if (!send_all && !amt.isNum()) {
                return Fail(ctx, OYO_ERR_INVALID_ARG, "amount_sat required (or send_all=true)");
            }
        }

        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w; op->kind = OpKind::MwebSend;
        op->send_all         = send_all;
        op->dry_run          = cfg["dry_run"].isBool() && cfg["dry_run"].get_bool();
        int32_t rc = ParseSendOutputsLocked(*op, ctx, cfg);
        if (rc != OYO_OK) return rc;
        // Custom change (P2.3) and drain output (P2.4) are canonical-send
        // only; MWEB change is a stealth output the builder manages.
        for (const auto& o : op->send_outputs)
            if (o.drain) return Fail(ctx, OYO_ERR_UNSUPPORTED,
                                     "max (drain) output not supported for MWEB sends");
        if (cfg["change_address"].isStr() && !cfg["change_address"].get_str().empty())
            return Fail(ctx, OYO_ERR_UNSUPPORTED,
                        "change_address not supported for MWEB sends");
        if (op->send_outputs.empty()) {
            op->mweb_send_to     = to.get_str();
            op->mweb_send_amount = send_all ? 0 : amt.get_int64();
        } else {
            op->mweb_send_to     = op->send_outputs[0].address;
            op->mweb_send_amount = op->send_outputs[0].amount_sat;
        }
        if (cfg["fee_rate_sat_per_vb"].isNum()) {
            int64_t r = cfg["fee_rate_sat_per_vb"].get_int64();
            if (r < 1) return Fail(ctx, OYO_ERR_INVALID_ARG, "fee_rate_sat_per_vb must be >= 1");
            op->fee_rate_sat_per_vb = static_cast<uint64_t>(r);
        }
        rc = ParseSendInputsLocked(*op, ctx, cfg, /*expect_mweb=*/true);
        if (rc != OYO_OK) return rc;
        StartMwebSendOp(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_ARG, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "mweb_wallet_send failed"); }
}

static int32_t mweb_bootstrap_impl(OYO_WALLET h, OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (!w->has_mweb || !w->mweb_keychain)
            return Fail(ctx, OYO_ERR_INVALID_STATE, "wallet has no mweb side");
        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w;
        op->kind = OpKind::MwebBootstrap;
        // Local-only since Slice 6: walks the persistent mirror, no RPC.
        // Synchronously sets state to OP_DONE before returning the handle.
        RunWalletBootstrapLocalLocked(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_STATE, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "mweb_wallet_bootstrap failed"); }
}

// ===========================================================================
// Unified wallet ABI (v7) — single open / send / new_address / etc. that
// dispatches by kind defines from oyoltc.h. The per-kind impl_* functions
// above hold the actual logic; this layer is just routing.
// ===========================================================================

OYO_API int32_t oyo_wallet_open(OYO_CHAIN chain, int32_t kind,
                                const uint8_t* wjson, size_t wlen,
                                OYO_WALLET* out) {
    switch (kind) {
        case OYO_WALLET_KIND_REGULAR:   return open_regular_impl(chain, wjson, wlen, out);
        case OYO_WALLET_KIND_MWEB:      return open_mweb_impl(chain, wjson, wlen, out);
        case OYO_WALLET_KIND_UNIVERSAL: return open_universal_impl(chain, wjson, wlen, out);
        case OYO_WALLET_KIND_WATCH:     return open_watch_impl(chain, wjson, wlen, out);
        default: {
            auto* c = AsChain(chain);
            return c ? Fail(c->ctx, OYO_ERR_INVALID_ARG, "unknown wallet kind")
                     : OYO_ERR_INVALID_HANDLE;
        }
    }
}

OYO_API int32_t oyo_wallet_new_address(OYO_WALLET wh, int32_t addr_kind,
                                        int32_t* out_index,
                                        const uint8_t** out_addr,
                                        size_t* out_len) {
    auto* w = AsWallet(wh);
    if (!w || !w->chain) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    // Auto-pick: pick the only side the wallet has. Universal must
    // disambiguate explicitly (P2WPKH or MWEB) — there's no sensible
    // default for "give me a new address" on a two-sided wallet.
    int32_t want = addr_kind;
    if (want == OYO_ADDR_AUTO) {
        if (w->has_p2wpkh && !w->has_mweb) want = OYO_ADDR_P2WPKH;
        else if (w->has_mweb && !w->has_p2wpkh) want = OYO_ADDR_MWEB;
        else return Fail(ctx, OYO_ERR_INVALID_ARG,
                          "addr_kind required (universal wallet has both sides)");
    }
    switch (want) {
        case OYO_ADDR_P2WPKH:
            if (!w->has_p2wpkh) return Fail(ctx, OYO_ERR_UNSUPPORTED, "wallet has no p2wpkh side");
            return new_canonical_address_impl(wh, "p2wpkh", out_index, out_addr, out_len);
        case OYO_ADDR_P2PKH:
            if (!w->has_p2wpkh) return Fail(ctx, OYO_ERR_UNSUPPORTED, "wallet has no canonical side");
            return new_canonical_address_impl(wh, "p2pkh", out_index, out_addr, out_len);
        case OYO_ADDR_P2SH_P2WPKH:
            if (!w->has_p2wpkh) return Fail(ctx, OYO_ERR_UNSUPPORTED, "wallet has no canonical side");
            return new_canonical_address_impl(wh, "p2sh-p2wpkh", out_index, out_addr, out_len);
        case OYO_ADDR_MWEB:
            if (!w->has_mweb) return Fail(ctx, OYO_ERR_UNSUPPORTED, "wallet has no mweb side");
            return new_mweb_address_impl(wh, out_index, out_addr, out_len);
        default:
            return Fail(ctx, OYO_ERR_INVALID_ARG, "unknown addr_kind");
    }
}

OYO_API int32_t oyo_wallet_add_address(OYO_WALLET wh,
                                        const uint8_t* address, size_t address_len,
                                        int32_t* out_binding_index) {
    auto* w = AsWallet(wh);
    if (!w || !w->chain) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    if (w->wallet_kind != "watch")
        return Fail(ctx, OYO_ERR_UNSUPPORTED,
                    "add_address is watch-only — derived wallets use new_address");
    return watch_add_address_impl(wh, address, address_len, out_binding_index);
}

OYO_API int32_t oyo_wallet_remove_address(OYO_WALLET wh, int32_t idx) {
    auto* w = AsWallet(wh);
    if (!w || !w->chain) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    if (w->wallet_kind != "watch")
        return Fail(ctx, OYO_ERR_UNSUPPORTED, "remove_address is watch-only");
    return watch_remove_address_impl(wh, idx);
}

OYO_API int32_t oyo_wallet_bootstrap(OYO_WALLET wh, OYO_OP* out) {
    auto* w = AsWallet(wh);
    if (!w || !w->chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    if (!w->has_mweb) {
        // No MWEB side — return a synchronously-Done op with skipped status
        // so callers can keep the same op-pump pattern across kinds.
        try {
            std::lock_guard<std::mutex> g(ctx->mu);
            auto op = std::make_unique<OyoOpImpl>();
            op->ctx = ctx;
            op->wallet = w;
            op->state = OYO_OP_DONE;
            UniValue r(UniValue::VOBJ);
            r.pushKV("status", "skipped");
            r.pushKV("reason", "wallet has no mweb side");
            op->result_json = r.write();
            OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
            ctx->ops.emplace(hp, std::move(op));
            *out = hp;
            return OYO_OK;
        } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    }
    return mweb_bootstrap_impl(wh, out);
}

// Unified send. Routing rule (symmetric — pick the side that has the
// balance, same shape for stealth and canonical destinations):
//   - watch wallets: reject (read-only).
//   - explicit inputs[]: route by the pinned coins' kind (MWEB commitment
//     → mweb_send; canonical txid:vout → regular/peg-in), not by balance.
//     Mixed canonical+MWEB pins are rejected.
//   - mweb-only wallet: mweb_send (handles both M→M and peg-out).
//   - regular-only wallet:
//       * canonical destination → regular_send.
//       * stealth destination   → pegin_send (R→M with canonical change).
//   - universal wallet (has both sides):
//       * canonical destination:
//           - canonical balance covers amount+budget → regular_send.
//           - else → mweb_send (peg-out from MWEB side).
//       * stealth destination:
//           - mweb balance covers amount+budget → mweb_send (M→M).
//           - else → pegin_send (R→M).
OYO_API int32_t oyo_wallet_send(OYO_WALLET wh,
                                 const uint8_t* config_json, size_t config_len,
                                 OYO_OP* out) {
    auto* w = AsWallet(wh);
    if (!w || !w->chain || !out) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = w->chain->ctx;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    if (w->wallet_kind == "watch") {
        return Fail(ctx, OYO_ERR_UNSUPPORTED, "watch wallets cannot send");
    }
    if (!config_json || !config_len) {
        return Fail(ctx, OYO_ERR_INVALID_ARG, "config_json required");
    }

    UniValue cfg;
    try { cfg = ParseJson(config_json, config_len); }
    catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INVALID_JSON, e.what()); }
    if (!cfg.isObject()) return Fail(ctx, OYO_ERR_INVALID_JSON, "config not object");

    // Single-recipient (legacy) vs multi-recipient (cfg.outputs[]). When
    // outputs[] is present it has been validated all same class by the
    // impl_send helper later; for routing we just need the first entry's
    // class. send_all and outputs[] are mutually exclusive (rejected
    // there too) so amount aggregation is straightforward.
    std::string route_addr;
    int64_t aggregate_amount_sat = 0;
    const UniValue& outs = cfg["outputs"];
    if (outs.isArray() && outs.size() > 0) {
        const UniValue& o0 = outs[0];
        if (!o0.isObject() || !o0["address"].isStr())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "outputs[0].address required");
        route_addr = o0["address"].get_str();
        for (size_t i = 0; i < outs.size(); ++i) {
            const UniValue& v = outs[i]["amount_sat"];
            if (v.isNum()) aggregate_amount_sat += v.get_int64();
        }
    } else {
        const UniValue& to = cfg["to"];
        if (!to.isStr() || to.get_str().empty())
            return Fail(ctx, OYO_ERR_INVALID_ARG, "to required");
        route_addr = to.get_str();
        if (cfg["amount_sat"].isNum()) aggregate_amount_sat = cfg["amount_sat"].get_int64();
    }
    CTxDestination dest = DecodeDestination(route_addr);
    if (!IsValidDestination(dest))
        return Fail(ctx, OYO_ERR_INVALID_ARG, "invalid destination: " + route_addr);
    const bool to_stealth = boost::get<StealthAddress>(&dest) != nullptr;

    // Explicitly-pinned inputs[] override the balance heuristic: the KIND
    // of coin the caller selected — not which side holds more balance —
    // fixes the send path. An MWEB commitment has to go through the MWEB
    // finalizer (M→M for a stealth dest, peg-out for a canonical dest); a
    // canonical txid:vout through regular/peg-in. Routing these by balance
    // handed MWEB commitments to regular_send on a both-sides-funded
    // universal wallet, which then rejected them with
    // "inputs[i] requires txid + vout (canonical path)".
    {
        const UniValue& ins = cfg["inputs"];
        if (ins.isArray() && ins.size() > 0) {
            bool saw_mweb = false, saw_canon = false;
            for (size_t i = 0; i < ins.size(); ++i) {
                const UniValue& e = ins[i];
                if (e.isObject() && e["commitment"].isStr() &&
                    !e["commitment"].get_str().empty())
                    saw_mweb = true;
                else
                    saw_canon = true;   // txid:vout — shape validated in the impl
            }
            // A single LTC tx crosses at most one MWEB boundary, so it
            // cannot spend canonical and MWEB inputs together.
            if (saw_mweb && saw_canon)
                return Fail(ctx, OYO_ERR_UNSUPPORTED,
                            "cannot mix canonical and MWEB inputs in one transaction");
            if (saw_mweb) {
                if (!w->has_mweb)
                    return Fail(ctx, OYO_ERR_INVALID_ARG,
                                "selected MWEB inputs but wallet has no MWEB side");
                return mweb_send_impl(wh, config_json, config_len, out);
            }
            if (!w->has_p2wpkh)
                return Fail(ctx, OYO_ERR_INVALID_ARG,
                            "selected canonical inputs but wallet has no canonical side");
            return to_stealth
                ? pegin_send_impl(wh, config_json, config_len, out)
                : regular_send_impl(wh, config_json, config_len, out);
        }
    }

    // mweb-only handles both stealth (M→M) and canonical (peg-out) inside.
    if (!w->has_p2wpkh) return mweb_send_impl(wh, config_json, config_len, out);

    // Regular-only wallet: canonical → spend, stealth → peg-in.
    if (!w->has_mweb) {
        return to_stealth
            ? pegin_send_impl(wh, config_json, config_len, out)
            : regular_send_impl(wh, config_json, config_len, out);
    }

    // Universal wallet — has both sides. Pick by which side has the
    // balance to cover (amount + budget). 100k sat slack matches
    // kMwebSendBudgetSat in the MWEB selector and absorbs auto-fee
    // jitter on the canonical side too.
    constexpr int64_t kPriorityBudgetSat = 100000;
    const bool send_all = cfg["send_all"].isBool() && cfg["send_all"].get_bool();
    const int64_t amount_sat = aggregate_amount_sat;
    // Canonical-only balance = unified - mweb (balance_sat is canonical
    // + mweb after NotifyMwebWalletLocked bumps both fields together).
    const int64_t canonical_balance = w->balance_sat - w->mweb_balance_sat;

    if (to_stealth) {
        // Prefer pure MWEB (cheaper, more private than peg-in).
        const bool prefer_mweb = send_all
            ? (w->mweb_balance_sat > 0)
            : (w->mweb_balance_sat >= amount_sat + kPriorityBudgetSat);
        if (prefer_mweb) return mweb_send_impl(wh, config_json, config_len, out);
        return pegin_send_impl(wh, config_json, config_len, out);
    }

    // Canonical destination. Prefer plain P2WPKH spend; fall back to
    // MWEB peg-out when the canonical side can't cover the send (e.g.
    // user funded only the MWEB half of a Universal wallet).
    const bool prefer_canonical = send_all
        ? (canonical_balance > 0)
        : (canonical_balance >= amount_sat + kPriorityBudgetSat);
    if (prefer_canonical) return regular_send_impl(wh, config_json, config_len, out);
    return mweb_send_impl(wh, config_json, config_len, out);
}


OYO_API int32_t oyo_wallet_rescan(OYO_WALLET h, OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w; op->kind = OpKind::Rescan;
        RunWalletRescanFromMirrorLocked(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "wallet_rescan failed"); }
}

OYO_API int32_t oyo_wallet_rescan_address(OYO_WALLET h, int32_t binding_index, OYO_OP* out) {
    auto* w = AsWallet(h);
    if (!w || !out) return OYO_ERR_INVALID_HANDLE;
    auto* chain = w->chain; auto* ctx = chain ? chain->ctx : nullptr;
    if (!ctx) return OYO_ERR_INVALID_HANDLE;
    try {
        std::lock_guard<std::mutex> g(ctx->mu);
        if (binding_index < 0 || size_t(binding_index) >= w->bindings.size() || !w->bindings[binding_index])
            return Fail(ctx, OYO_ERR_INVALID_ARG, "binding_index out of range or removed");
        auto op = std::make_unique<OyoOpImpl>();
        op->ctx = ctx; op->chain = chain; op->wallet = w; op->kind = OpKind::Rescan;
        op->rescan_scripts.push_back(w->bindings[binding_index]->addr->script_hex);
        RunWalletRescanFromMirrorLocked(*op);
        OYO_OP hp = reinterpret_cast<OYO_OP>(op.get());
        ctx->ops.emplace(hp, std::move(op));
        *out = hp;
        return OYO_OK;
    } catch (const std::exception& e) { return Fail(ctx, OYO_ERR_INTERNAL, e.what()); }
    catch (...) { return Fail(ctx, OYO_ERR_INTERNAL, "wallet_rescan_address failed"); }
}

OYO_API int32_t oyo_op_state(OYO_OP h, int32_t* out_state) {
    auto* op = AsOp(h);
    if (!op || !out_state) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = op->ctx; if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    *out_state = op->state;
    return OYO_OK;
}

OYO_API int32_t oyo_op_rpc_request(OYO_OP h, const uint8_t** out_json, size_t* out_len) {
    auto* op = AsOp(h);
    if (!op || !out_json || !out_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = op->ctx; if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    if (op->state != OYO_OP_NEED_RPC) return OYO_ERR_INVALID_STATE;
    *out_json = reinterpret_cast<const uint8_t*>(op->rpc_req.data());
    *out_len = op->rpc_req.size();
    return OYO_OK;
}

OYO_API int32_t oyo_op_provide_rpc_response(OYO_OP h, const uint8_t* resp, size_t rlen) {
    auto* op = AsOp(h);
    if (!op) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = op->ctx; if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    if (op->state != OYO_OP_NEED_RPC) return OYO_ERR_INVALID_STATE;
    try {
        UniValue env = ParseJson(resp, rlen);
        switch (op->kind) {
            case OpKind::Sync:            DispatchSync(*op, env); break;
            case OpKind::Rescan:          break; /* synchronous mirror-rescan, no RPC dispatch */
            case OpKind::MempoolSync:     DispatchMempool(*op, env); break;
            case OpKind::MwebBootstrap:   /* local-only since Slice 6, no RPC dispatch */ break;
            case OpKind::MwebSend:        DispatchMwebSend(*op, env); break;
            case OpKind::RegularSend:     DispatchRegularSend(*op, env); break;
            case OpKind::ExtPegIn:        DispatchExtPegIn(*op, env); break;
        }
        return OYO_OK;
    } catch (const std::exception& e) {
        op->err_code = OYO_ERR_RPC; op->err_msg = e.what(); op->state = OYO_OP_FAILED;
        // Mirror to ctx-level last_error so the Go cgo wrapper's lastErr()
        // (which reads from ctx) can surface a useful message instead of
        // just rc=-8.
        ctx->last_err_code = OYO_ERR_RPC;
        ctx->last_err_msg  = e.what();
        UniValue r(UniValue::VOBJ);
        r.pushKV("status", "error"); r.pushKV("error", op->err_msg);
        op->result_json = r.write();
        return OYO_ERR_RPC;
    } catch (...) {
        op->err_code = OYO_ERR_INTERNAL; op->err_msg = "provide_rpc_response failed"; op->state = OYO_OP_FAILED;
        ctx->last_err_code = OYO_ERR_INTERNAL;
        ctx->last_err_msg  = "provide_rpc_response failed";
        return OYO_ERR_INTERNAL;
    }
}

OYO_API int32_t oyo_op_result(OYO_OP h, const uint8_t** out_json, size_t* out_len) {
    auto* op = AsOp(h);
    if (!op || !out_json || !out_len) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = op->ctx; if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    *out_json = reinterpret_cast<const uint8_t*>(op->result_json.data());
    *out_len = op->result_json.size();
    return OYO_OK;
}

OYO_API int32_t oyo_op_close(OYO_OP h) {
    auto* op = AsOp(h);
    if (!op) return OYO_ERR_INVALID_HANDLE;
    auto* ctx = op->ctx; if (!ctx) return OYO_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> g(ctx->mu);
    auto it = ctx->ops.find(h);
    if (it == ctx->ops.end()) return OYO_ERR_NOT_FOUND;
    it->second->magic = kMagicDead;
    ctx->ops.erase(it);
    return OYO_OK;
}

} // extern "C"
