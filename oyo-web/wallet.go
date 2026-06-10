package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"oyo-web/oyoltc"
)

const maxChainReorgDepth = 5000

// WalletRegistry is a thin Go-level adapter over the liboyoltc chain context.
// All wallet state — derivation, UTXO tracking, balances, rescan, sync — lives
// inside the C library; the registry only owns lifecycles and dispatches RPC
// requests produced by op pumps to the node.
type WalletRegistry struct {
	mu      sync.Mutex
	ctx     *oyoltc.Context
	chain   *oyoltc.Chain
	rpc     *RPCClient
	wallets map[string]*Wallet

	// Init params kept around so Resync can reopen ctx+chain with
	// the same configuration after wiping the on-disk mirror.
	network string
	workdir string
	// rollbackWindow: in-memory fast-reorg-undo window forwarded to
	// chain_open (kept here so Resync reopens with the same value).
	// Resolved to 100 when unset; liboyoltc clamps the effective value to 1000.
	rollbackWindow int
	// nativeBlockParse: forward raw-block native parse to chain_open (#5);
	// kept here so Resync reopens with the same value.
	nativeBlockParse bool

	// Identity check between our chain mirror and whatever node is
	// answering at cfg.RPC. Set on first successful tailer pass; if a
	// later pass sees a different genesis we mark `desync` and stop
	// driving sync ops — running against a foreign chain would corrupt
	// our UTXO view. Operator must manually Resync once they confirm
	// the node is the right one.
	genesisHash  string
	desync       bool
	desyncReason string

	trackMempool bool
	syncMu       sync.Mutex    // serializes chain.Sync / mempool sync runs
	tailQuit     chan struct{} // close to stop the tailer
	tailWake     chan struct{} // poke for an immediate sync

	prefetchMu    sync.Mutex
	prefetchStats syncPrefetchStats
}

// NewWalletRegistry initialises liboyoltc and opens a chain context for the
// given network ("regtest" / "test" / "main"). rpc is used by op pumps later.
// trackMempool enables the per-tick mempool sync path; off by default for
// safety on mainnet (large mempools mean many getrawtransaction calls before
// the seen-not-ours cache fills). workdir is forwarded to oyoltc.Open and
// determines whether the per-chain MWEB UTXO mirror lives on disk
// (workdir != "") or in :memory: (workdir == "").
func NewWalletRegistry(rpc *RPCClient, network, workdir string, trackMempool bool, rollbackWindow int, nativeBlockParse bool) (*WalletRegistry, error) {
	if network == "" {
		network = "regtest"
	}
	if rollbackWindow <= 0 {
		rollbackWindow = 100
	}
	ctx, err := oyoltc.Open(workdir)
	if err != nil {
		return nil, fmt.Errorf("oyoltc open: %w", err)
	}
	cfg, _ := json.Marshal(map[string]interface{}{
		"network":            network,
		"rollback_window":    rollbackWindow,
		"max_reorg_depth":    maxChainReorgDepth,
		"track_mempool":      trackMempool,
		"native_block_parse": nativeBlockParse,
	})
	chain, err := ctx.ChainOpen(cfg)
	if err != nil {
		_ = ctx.Close()
		return nil, fmt.Errorf("oyoltc chain_open: %w", err)
	}
	return &WalletRegistry{
		ctx:              ctx,
		chain:            chain,
		rpc:              rpc,
		wallets:          make(map[string]*Wallet),
		network:          network,
		workdir:          workdir,
		trackMempool:     trackMempool,
		rollbackWindow:   rollbackWindow,
		nativeBlockParse: nativeBlockParse,
	}, nil
}

func (r *WalletRegistry) Close() error {
	r.StopTailer()
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, w := range r.wallets {
		if w.handle != nil {
			_ = w.handle.Close()
		}
	}
	r.wallets = nil
	if r.chain != nil {
		_ = r.chain.Close()
		r.chain = nil
	}
	if r.ctx != nil {
		_ = r.ctx.Close()
		r.ctx = nil
	}
	return nil
}

// SyncOnce drives chain.Sync to completion through the RPC client. Idempotent
// and serialised — concurrent callers wait on syncMu, but no-op if a sync is
// already in flight (returned immediately by trylock semantics is overkill;
// just block — sync is short for typical incremental advances).
func (r *WalletRegistry) SyncOnce() (json.RawMessage, error) {
	r.syncMu.Lock()
	defer r.syncMu.Unlock()
	if r.chain == nil {
		return nil, errors.New("chain closed")
	}
	op, err := r.chain.Sync()
	if err != nil {
		return nil, err
	}
	defer op.Close()
	rpcFunc := r.rpc.CallJSON
	if pf := r.newSyncPrefetcher(); pf != nil {
		defer func() {
			pf.Close()
			r.setSyncPrefetchStats(pf.Snapshot())
		}()
		rpcFunc = pf.CallJSON
	} else {
		r.setSyncPrefetchStats(syncPrefetchStats{Enabled: false})
	}
	res, err := oyoltc.RunOp(op, rpcFunc)
	return res, err
}

func (r *WalletRegistry) setSyncPrefetchStats(stats syncPrefetchStats) {
	r.prefetchMu.Lock()
	defer r.prefetchMu.Unlock()
	r.prefetchStats = stats
}

func (r *WalletRegistry) SyncPrefetchStats() syncPrefetchStats {
	r.prefetchMu.Lock()
	defer r.prefetchMu.Unlock()
	return r.prefetchStats
}

func (r *WalletRegistry) newSyncPrefetcher() *syncPrefetcher {
	if r.rpc == nil || r.chain == nil {
		return nil
	}
	statusRaw, err := r.chain.StatusJSON()
	if err != nil {
		return nil
	}
	var status struct {
		TipHeight int64 `json:"tip_height"`
	}
	if err := json.Unmarshal(statusRaw, &status); err != nil || status.TipHeight < -1 {
		return nil
	}
	chainRaw, err := r.rpc.CallRaw("getblockchaininfo", nil)
	if err != nil {
		return nil
	}
	var chain struct {
		Blocks int64 `json:"blocks"`
	}
	if err := json.Unmarshal(chainRaw, &chain); err != nil || chain.Blocks <= status.TipHeight {
		return nil
	}
	verbosity := int64(2)
	if r.nativeBlockParse {
		verbosity = 0
	}
	return newSyncPrefetcher(r.rpc, status.TipHeight+1, chain.Blocks, verbosity)
}

// MempoolSyncOnce drives a single chain.MempoolSync pass. No-op (returns
// {"status":"disabled"}) if track_mempool was not enabled in chain config.
func (r *WalletRegistry) MempoolSyncOnce() (json.RawMessage, error) {
	r.syncMu.Lock()
	defer r.syncMu.Unlock()
	if r.chain == nil {
		return nil, errors.New("chain closed")
	}
	op, err := r.chain.MempoolSync()
	if err != nil {
		return nil, err
	}
	defer op.Close()
	res, err := oyoltc.RunOp(op, r.rpc.CallJSON)
	return res, err
}

// ChainStatusJSON returns the liboyoltc chain status snapshot
// (tip_height, blocks_cached, addresses_tracked, mempool counts,
// MWEB-mirror stats, registered wallet names). Backs /api/syncing.
func (r *WalletRegistry) ChainStatusJSON() (json.RawMessage, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.chain == nil {
		return nil, errors.New("chain closed")
	}
	return r.chain.StatusJSON()
}

// AddressStatusJSON returns the mirror-derived confirmed/immature balance and
// unspent set for an arbitrary canonical address — computed locally from the
// full regular-UTXO mirror, zero node queries. address is a base58/bech32
// string; MWEB stealth addresses are rejected by liboyoltc (the caller routes
// those through the owning wallet's scan_secret instead).
func (r *WalletRegistry) AddressStatusJSON(address string, history bool, limit int) (json.RawMessage, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.chain == nil {
		return nil, errors.New("chain closed")
	}
	req := map[string]interface{}{"address": address, "history": history}
	if limit > 0 {
		req["limit"] = limit // explorer "load more"; absent → engine default window
	}
	m, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}
	return r.chain.AddressStatus(m)
}

// Resync wipes the on-disk MWEB UTXO mirror and rebuilds chain state
// in-process. No exit / restart involved. Sequence:
//
//  1. Stop the tailer (fence concurrent sync runs).
//  2. Close every wallet handle (they hold pointers into the chain).
//  3. Close chain and ctx (flushes the persistent mirror).
//  4. Delete oyoltc-*.db* files and oyoltc-*.ldb dirs in r.workdir.
//  5. Re-open ctx + chain with the same network / track_mempool config.
//  6. Re-create each registered wallet's handle from saved seed/kind.
//  7. Drive each wallet's bootstrap op (scantxoutset / MWEB rescan).
//  8. Restart the tailer.
//
// Returns { removed_files, wallets_reopened }. Wallet seeds and the
// in-memory registry survive — a Resync drops chain state, not user
// configuration.
func (r *WalletRegistry) Resync() (map[string]interface{}, error) {
	r.StopTailer()
	r.mu.Lock()
	defer r.mu.Unlock()

	// Clear identity-check state. The next tailer pass picks up a
	// fresh genesis_hash from whichever node is at cfg.RPC now;
	// desync flag clears with it.
	r.genesisHash = ""
	r.desync = false
	r.desyncReason = ""

	// 1. Close all wallet handles (chain.Close requires no live wallets).
	for _, w := range r.wallets {
		w.mu.Lock()
		if w.handle != nil {
			_ = w.handle.Close()
			w.handle = nil
		}
		w.loaded = false
		w.mu.Unlock()
	}

	// 2. Close chain + ctx so the mirror is flushed and safe to delete.
	if r.chain != nil {
		_ = r.chain.Close()
		r.chain = nil
	}
	if r.ctx != nil {
		_ = r.ctx.Close()
		r.ctx = nil
	}

	// 3. Wipe on-disk mirror files.
	matches, _ := filepath.Glob(filepath.Join(r.workdir, "oyoltc-*.db*"))
	for _, f := range matches {
		_ = os.Remove(f)
	}
	ldbMatches, _ := filepath.Glob(filepath.Join(r.workdir, "oyoltc-*.ldb"))
	for _, f := range ldbMatches {
		_ = os.RemoveAll(f)
	}

	// 4. Reopen ctx + chain with the same config we were created with.
	ctx, err := oyoltc.Open(r.workdir)
	if err != nil {
		return nil, fmt.Errorf("resync open ctx: %w", err)
	}
	cfg, _ := json.Marshal(map[string]interface{}{
		"network":            r.network,
		"rollback_window":    r.rollbackWindow,
		"max_reorg_depth":    maxChainReorgDepth,
		"track_mempool":      r.trackMempool,
		"native_block_parse": r.nativeBlockParse,
	})
	chain, err := ctx.ChainOpen(cfg)
	if err != nil {
		_ = ctx.Close()
		return nil, fmt.Errorf("resync open chain: %w", err)
	}
	r.ctx = ctx
	r.chain = chain

	// 5. The re-opened chain has an empty mirror; it self-seeds by walking
	//    from genesis on the next sync, then per-wallet bootstrap reads it.
	//    No node-side seed (scantxoutset/listutxos) needed.

	// 6. Re-create wallet handles from preserved metadata.
	reopened := make([]string, 0, len(r.wallets))
	for _, w := range r.wallets {
		w.mu.Lock()
		err := r.reopenWalletLocked(w)
		w.mu.Unlock()
		if err != nil {
			log.Printf("resync: reopen %s failed: %v", w.Name, err)
			continue
		}
		reopened = append(reopened, w.Name)
	}
	sort.Strings(reopened)

	// 7. Bootstrap each wallet so its UTXO view is rebuilt against
	//    the fresh chain state. Regular / universal canonical side via
	//    scantxoutset; MWEB side via the keychain's bootstrap op (which
	//    walks the freshly-seeded MWEB mirror).
	for _, w := range r.wallets {
		w.mu.Lock()
		var bootOps []*oyoltc.Op
		if w.handle != nil {
			if w.Kind == "regular" || w.Kind == "universal" {
				op, err := w.handle.Rescan()
				if err == nil {
					bootOps = append(bootOps, op)
				}
			}
			if w.Kind == "mweb" || w.Kind == "universal" {
				op, err := w.handle.Bootstrap()
				if err == nil {
					bootOps = append(bootOps, op)
				}
			}
		}
		w.mu.Unlock()
		for _, op := range bootOps {
			_, opErr := oyoltc.RunOp(op, r.rpc.CallJSON)
			if opErr != nil {
				log.Printf("resync: bootstrap %s failed: %v", w.Name, opErr)
			}
			_ = op.Close()
		}
	}

	// 7. Restart the tailer with the same interval the API set up.
	go r.StartTailer(3 * time.Second)

	return map[string]interface{}{
		"removed_files":    matches,
		"wallets_reopened": reopened,
	}, nil
}

// StartTailer launches a background goroutine that drives chain.Sync at the
// given interval. WakeTailer can be called to trigger an immediate sync (e.g.
// after a successful send / mine in the same process).
func (r *WalletRegistry) StartTailer(interval time.Duration) {
	r.mu.Lock()
	if r.tailQuit != nil {
		r.mu.Unlock()
		return
	}
	r.tailQuit = make(chan struct{})
	r.tailWake = make(chan struct{}, 1)
	quit := r.tailQuit
	wake := r.tailWake
	r.mu.Unlock()

	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-quit:
				return
			case <-t.C:
			case <-wake:
			}
			// Identity gate before any sync work touches our UTXO mirror
			// against a foreign chain. Same-genesis reorgs are handled by
			// liboyoltc's persistent rollback, bounded by max_reorg_depth.
			if action := r.checkChainIntegrity(); action == "desync" {
				continue // hard stop until manual Resync
			}
			chainSynced := false
			for {
				res, err := r.SyncOnce()
				if err != nil {
					log.Printf("oyo chain sync: %v", err)
					if r.isMaxReorgDepthError(res, err) {
						r.markDesync("chain reorg exceeds max_reorg_depth; refusing automatic rollback/resync")
					}
					break
				}
				var st struct {
					Status string `json:"status"`
					Error  string `json:"error"`
				}
				if err := json.Unmarshal(res, &st); err != nil {
					log.Printf("oyo chain sync: invalid status response: %v", err)
					break
				}
				switch st.Status {
				case "partial":
					select {
					case <-quit:
						return
					default:
					}
					continue
				case "synced":
					chainSynced = true
				case "error":
					log.Printf("oyo chain sync error: %s", st.Error)
				default:
					log.Printf("oyo chain sync: unexpected status %q", st.Status)
				}
				break
			}
			if r.trackMempool && chainSynced {
				// One pass drives both canonical and MWEB pending (MWEB is
				// folded into the same getrawtransaction walk).
				if _, err := r.MempoolSyncOnce(); err != nil {
					log.Printf("oyo mempool sync: %v", err)
				}
			}
		}
	}()
}

// checkChainIntegrity verifies that the node we're talking to still
// belongs to the same chain we've been mirroring.
//
// Returns one of:
//
//	""             — all good, proceed with sync ops
//	"desync"       — node has a different genesis (probably swapped
//	                 to a different network or wallet pointed at the
//	                 wrong RPC); set r.desync, do nothing
//
// Errors fetching from the node return "" (best-effort; transient
// node hiccups don't trip the gate).
func (r *WalletRegistry) checkChainIntegrity() string {
	// 1. Genesis match (cheap, catches "wrong node" instantly).
	g, err := r.rpc.CallRaw("getblockhash", []interface{}{0})
	if err != nil {
		return ""
	}
	var nodeGenesis string
	_ = json.Unmarshal(g, &nodeGenesis)
	if nodeGenesis == "" {
		return ""
	}
	r.mu.Lock()
	if r.genesisHash == "" {
		r.genesisHash = nodeGenesis
	}
	if nodeGenesis != r.genesisHash {
		r.desync = true
		r.desyncReason = fmt.Sprintf("genesis hash changed: had %s, node now reports %s", r.genesisHash, nodeGenesis)
		log.Printf("Desync: %s", r.desyncReason)
		r.mu.Unlock()
		return "desync"
	}
	if r.desync {
		// Was previously marked desync but caller fixed it (or genesis
		// matched after a flap). Keep the flag — operator decides when
		// to clear via Resync.
		r.mu.Unlock()
		return "desync"
	}
	r.mu.Unlock()
	return ""
}

func (r *WalletRegistry) markDesync(reason string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.desync = true
	r.desyncReason = reason
	log.Printf("Desync: %s", r.desyncReason)
}

func (r *WalletRegistry) isMaxReorgDepthError(res json.RawMessage, err error) bool {
	if err != nil && strings.Contains(err.Error(), "max_reorg_depth") {
		return true
	}
	return strings.Contains(string(res), "max_reorg_depth")
}

// DesyncStatus returns the current desync flag + reason. Surfaced
// through /api/syncing so the frontend can banner the operator.
func (r *WalletRegistry) DesyncStatus() (bool, string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.desync, r.desyncReason
}

// WakeTailer requests an immediate sync. Non-blocking — if a wake is already
// queued, this is a no-op.
func (r *WalletRegistry) WakeTailer() {
	r.mu.Lock()
	w := r.tailWake
	r.mu.Unlock()
	if w == nil {
		return
	}
	select {
	case w <- struct{}{}:
	default:
	}
}

func (r *WalletRegistry) StopTailer() {
	r.mu.Lock()
	q := r.tailQuit
	r.tailQuit = nil
	r.tailWake = nil
	r.mu.Unlock()
	if q != nil {
		close(q)
	}
}

// CreateWatch opens an empty watch-only wallet in the registry.
func (r *WalletRegistry) CreateWatch(name string) (*Wallet, error) {
	if name == "" {
		return nil, errors.New("wallet name required")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.wallets[name]; ok {
		return nil, fmt.Errorf("OYO wallet %q already exists", name)
	}
	cfg, _ := json.Marshal(map[string]interface{}{"name": name})
	h, err := r.chain.OpenWallet(oyoltc.KindWatch, cfg)
	if err != nil {
		return nil, err
	}
	w := &Wallet{Name: name, Kind: "watch", registry: r, handle: h, loaded: true}
	r.wallets[name] = w
	return w, nil
}

// AddWatchAddress decodes and registers an address on a watch-only wallet.
// Returns the resulting binding index.
//
// Records the address in w.watchAddresses too so a subsequent
// Unload→Load cycle re-adds it without losing the binding.
func (w *Wallet) AddWatchAddress(address string) (int32, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if !w.loaded || w.handle == nil {
		return 0, errUnloadedWallet
	}
	idx, err := w.handle.AddAddress(address)
	if err == nil {
		w.watchAddresses = append(w.watchAddresses, address)
	}
	return idx, err
}

// CreateMweb opens an MWEB OYO wallet. seed is a free-form string —
// liboyoltc hashes it with the "oyo_mweb_v1_seed" domain tag to derive
// the 32-byte master, then runs the existing scan/spend chain. Same UX
// shape as Regular wallets (no 64-hex requirement). Wallet starts with
// no addresses; allocate via NewMwebAddress before any output match.
func (r *WalletRegistry) CreateMweb(name, seed string, birthHeight int64) (*Wallet, error) {
	if name == "" {
		return nil, errors.New("wallet name required")
	}
	if seed == "" {
		return nil, errors.New("seed string required")
	}
	if birthHeight < 0 {
		return nil, errors.New("birth_height must be >= 0")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.wallets[name]; ok {
		return nil, fmt.Errorf("OYO wallet %q already exists", name)
	}
	cfg, _ := json.Marshal(map[string]interface{}{
		"name":         name,
		"seed_type":    "oyo_mweb_v1",
		"seed":         seed,
		"birth_height": birthHeight,
	})
	h, err := r.chain.OpenWallet(oyoltc.KindMweb, cfg)
	if err != nil {
		return nil, err
	}
	w := &Wallet{
		Name: name, Kind: "mweb", registry: r, handle: h, loaded: true,
		seedString:  seed,
		birthHeight: birthHeight,
	}
	r.wallets[name] = w
	return w, nil
}

// liveHandle returns the wallet's liboyoltc handle if loaded, or
// errUnloadedWallet otherwise. Used by the Op-builder methods so a
// caller dispatching against an unloaded wallet sees a clear error
// rather than a nil-deref.
func (w *Wallet) liveHandle() (*oyoltc.Wallet, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if !w.loaded || w.handle == nil {
		return nil, errUnloadedWallet
	}
	return w.handle, nil
}

// NewAddress allocates the next derived address. addrKind is one of
// oyoltc.AddrAuto / oyoltc.AddrP2wpkh / oyoltc.AddrMweb. AUTO picks the
// only side a single-side wallet has; universal must disambiguate.
//
// For MWEB allocations on mweb/universal wallets, tracks the running
// count so a future Unload→Load cycle can re-derive the same indices
// (the C lib doesn't auto-replay on OpenWallet).
func (w *Wallet) NewAddress(addrKind int32) (int32, string, error) {
	h, err := w.liveHandle()
	if err != nil {
		return 0, "", err
	}
	idx, addr, allocErr := h.NewAddress(addrKind)
	if allocErr == nil && (addrKind == oyoltc.AddrMweb ||
		(addrKind == oyoltc.AddrAuto && w.Kind == "mweb")) {
		w.mu.Lock()
		w.mwebAddressCount++
		w.mu.Unlock()
	}
	return idx, addr, allocErr
}

// BootstrapOp returns the unified MWEB-bootstrap op. No-op (Done
// immediately) for wallets without an MWEB side; the C library
// returns a {"status":"skipped"} result so callers stay uniform.
func (w *Wallet) BootstrapOp() *WalletOp {
	return &WalletOp{wallet: w, open: func() (*oyoltc.Op, error) {
		h, err := w.liveHandle()
		if err != nil {
			return nil, err
		}
		return h.Bootstrap()
	}}
}

// SendOp returns the unified send op. C-side dispatch picks regular /
// mweb / pegin based on wallet kind + destination class — see
// oyoltc.h's oyo_wallet_send doc.
func (w *Wallet) SendOp(cfgJSON []byte) *WalletOp {
	cfgCopy := append([]byte(nil), cfgJSON...)
	return &WalletOp{wallet: w, open: func() (*oyoltc.Op, error) {
		h, err := w.liveHandle()
		if err != nil {
			return nil, err
		}
		return h.Send(cfgCopy)
	}}
}

// CreateUniversal opens a universal OYO wallet (both P2WPKH and MWEB
// sides driven by a single 32-byte master seed). kinds may be empty (both
// kinds enabled), or contain a subset like ["p2wpkh"] or ["mweb"] to cap
// the wallet to one side only — useful for receive-only deployments.
//
// The resulting Wallet.Kind is:
//   - "universal" when both sides are enabled
//   - "regular"   when only p2wpkh is enabled
//   - "mweb"      when only mweb is enabled
//
// Send routing in handleWalletSend uses the Kind + destination class to
// pick the right liboyoltc op (RegularSend / MwebSend / ExtPegInSend).
func (r *WalletRegistry) CreateUniversal(name, seed string, kinds []string, addressCount int, birthHeight int64) (*Wallet, error) {
	if name == "" {
		return nil, errors.New("wallet name required")
	}
	if seed == "" {
		return nil, errors.New("seed string required")
	}
	if addressCount <= 0 {
		addressCount = 8
	}
	if addressCount > 1000 {
		return nil, errors.New("address_count too large")
	}
	if birthHeight < 0 {
		return nil, errors.New("birth_height must be >= 0")
	}
	hasP2wpkh, hasMweb := true, true
	if len(kinds) > 0 {
		hasP2wpkh, hasMweb = false, false
		for _, k := range kinds {
			switch k {
			case "p2wpkh":
				hasP2wpkh = true
			case "mweb":
				hasMweb = true
			default:
				return nil, fmt.Errorf("unknown kind: %s", k)
			}
		}
		if !hasP2wpkh && !hasMweb {
			return nil, errors.New("at least one kind required")
		}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.wallets[name]; ok {
		return nil, fmt.Errorf("OYO wallet %q already exists", name)
	}
	cfg := map[string]interface{}{
		"name":          name,
		"seed":          seed,
		"address_count": addressCount,
		"birth_height":  birthHeight,
	}
	if len(kinds) > 0 {
		cfg["kinds"] = kinds
	}
	cfgBytes, _ := json.Marshal(cfg)
	h, err := r.chain.OpenWallet(oyoltc.KindUniversal, cfgBytes)
	if err != nil {
		return nil, err
	}
	walletKind := "universal"
	if hasP2wpkh && !hasMweb {
		walletKind = "regular"
	} else if !hasP2wpkh && hasMweb {
		walletKind = "mweb"
	}
	wlt := &Wallet{
		Name: name, Kind: walletKind, registry: r, handle: h, loaded: true,
		seedString:   seed,
		addressCount: addressCount,
		birthHeight:  birthHeight,
		kinds:        append([]string(nil), kinds...),
	}
	r.wallets[name] = wlt
	return wlt, nil
}

func (r *WalletRegistry) CreateRegular(name, seed string, addressCount int) (*Wallet, error) {
	if name == "" {
		return nil, errors.New("wallet name required")
	}
	if seed == "" {
		return nil, errors.New("seed string required")
	}
	if addressCount <= 0 {
		addressCount = 8
	}
	if addressCount > 1000 {
		return nil, errors.New("address_count too large")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.wallets[name]; ok {
		return nil, fmt.Errorf("OYO wallet %q already exists", name)
	}
	cfg, _ := json.Marshal(map[string]interface{}{
		"name":          name,
		"seed_type":     "oyo_v1",
		"seed":          seed,
		"address_count": addressCount,
		"address_kind":  "p2wpkh",
	})
	h, err := r.chain.OpenWallet(oyoltc.KindRegular, cfg)
	if err != nil {
		return nil, err
	}
	w := &Wallet{
		Name: name, Kind: "regular", registry: r, handle: h, loaded: true,
		seedString:   seed,
		addressCount: addressCount,
	}
	r.wallets[name] = w
	return w, nil
}

func (r *WalletRegistry) Get(name string) (*Wallet, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	w, ok := r.wallets[name]
	return w, ok
}

// Delete unregisters a wallet permanently — closes the liboyoltc handle
// (if loaded) and drops the entry from the registry. The seed material
// in RAM is gone after this call. Returns false if no such wallet.
func (r *WalletRegistry) Delete(name string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	w, ok := r.wallets[name]
	if !ok {
		return false
	}
	if w.handle != nil {
		_ = w.handle.Close()
	}
	delete(r.wallets, name)
	return true
}

// Unload closes the liboyoltc handle but keeps the wallet entry +
// creation params in the registry so Load can reopen it without the
// user re-entering the seed. Returns:
//   - "unloaded" — handle was open, now closed
//   - "already_unloaded" — entry exists but no handle
//   - "missing" — no such wallet
//
// Watch wallets snapshot their imported addresses from the live status
// before closing the handle so Load can re-add them in the same order
// (binding indices stay stable across the cycle).
func (r *WalletRegistry) Unload(name string) string {
	r.mu.Lock()
	defer r.mu.Unlock()
	w, ok := r.wallets[name]
	if !ok {
		return "missing"
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if !w.loaded {
		return "already_unloaded"
	}
	if w.Kind == "watch" {
		// Read the current address list before closing — there's no
		// other source of truth for runtime-imported watch addresses.
		var st struct {
			Addresses []struct {
				Index   int    `json:"index"`
				Address string `json:"address"`
			} `json:"addresses"`
		}
		if js := w.snapshotLocked(); js != nil {
			_ = json.Unmarshal(js, &st)
		}
		// Preserve in binding-index order.
		sort.Slice(st.Addresses, func(i, j int) bool { return st.Addresses[i].Index < st.Addresses[j].Index })
		w.watchAddresses = w.watchAddresses[:0]
		for _, a := range st.Addresses {
			w.watchAddresses = append(w.watchAddresses, a.Address)
		}
	}
	// Cache the last status before closing so /api/wallets can still
	// render a row for this wallet while it's unloaded.
	_ = w.snapshotLocked()
	_ = w.handle.Close()
	w.handle = nil
	w.rev = 0
	w.loaded = false
	return "unloaded"
}

// reopenWalletLocked rebuilds a wallet's liboyoltc handle from the
// preserved creation params (kind, seed, address counts, watch
// addresses). Caller must hold r.mu and w.mu. The wallet must be in
// the !loaded state — caller is responsible for closing the old
// handle (if any) first.
func (r *WalletRegistry) reopenWalletLocked(w *Wallet) error {
	var kindCode int32
	var cfgBytes []byte
	switch w.Kind {
	case "watch":
		kindCode = oyoltc.KindWatch
		cfgBytes, _ = json.Marshal(map[string]interface{}{"name": w.Name})
	case "mweb":
		kindCode = oyoltc.KindMweb
		cfgBytes, _ = json.Marshal(map[string]interface{}{
			"name":         w.Name,
			"seed_type":    "oyo_mweb_v1",
			"seed":         w.seedString,
			"birth_height": w.birthHeight,
		})
	case "regular":
		kindCode = oyoltc.KindRegular
		cfgBytes, _ = json.Marshal(map[string]interface{}{
			"name":          w.Name,
			"seed_type":     "oyo_v1",
			"seed":          w.seedString,
			"address_count": w.addressCount,
			"address_kind":  "p2wpkh",
		})
	case "universal":
		kindCode = oyoltc.KindUniversal
		cfg := map[string]interface{}{
			"name":          w.Name,
			"seed":          w.seedString,
			"address_count": w.addressCount,
			"birth_height":  w.birthHeight,
		}
		if len(w.kinds) > 0 {
			cfg["kinds"] = w.kinds
		}
		cfgBytes, _ = json.Marshal(cfg)
	default:
		return fmt.Errorf("unknown wallet kind: %s", w.Kind)
	}
	h, err := r.chain.OpenWallet(kindCode, cfgBytes)
	if err != nil {
		return err
	}
	w.handle = h
	w.loaded = true
	w.rev = 0
	if w.Kind == "watch" {
		for _, addr := range w.watchAddresses {
			if _, addErr := w.handle.AddAddress(addr); addErr != nil {
				log.Printf("watch address re-add failed for %s: %v", addr, addErr)
			}
		}
	}
	if w.Kind == "mweb" || w.Kind == "universal" {
		for i := 0; i < w.mwebAddressCount; i++ {
			if _, _, addErr := w.handle.NewAddress(oyoltc.AddrMweb); addErr != nil {
				log.Printf("mweb address re-alloc failed for %s idx=%d: %v", w.Name, i, addErr)
				break
			}
		}
	}
	return nil
}

// Load reopens a previously-unloaded wallet from its preserved creation
// params. Idempotent on already-loaded wallets. Returns "missing" if
// there's no entry, or surfaces the underlying liboyoltc error.
func (r *WalletRegistry) Load(name string) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	w, ok := r.wallets[name]
	if !ok {
		return "missing", nil
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.loaded {
		return "already_loaded", nil
	}
	if err := r.reopenWalletLocked(w); err != nil {
		return "", err
	}
	return "loaded", nil
}

func (r *WalletRegistry) List() []*Wallet {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]*Wallet, 0, len(r.wallets))
	for _, w := range r.wallets {
		out = append(out, w)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// Wallet wraps a liboyoltc wallet handle. All accessors render JSON
// from the library's revision-aware status; per-method shapes preserve the
// pre-refactor HTTP API surface.
//
// Lifecycle: a wallet can be Loaded (handle != nil, receiving chain
// updates) or Unloaded (handle nil, but the original creation params
// preserved on the struct so Load() can reopen it). Unload is reversible
// — the wallet is still in the registry and visible in /api/wallets, but
// op endpoints (send/newaddress/...) reject it. Delete is the only
// permanent removal: closes the handle if open and drops the entry.
//
// All persistence is in-memory only: a process restart loses all OYO
// wallets (loaded or unloaded). Users back up the seed externally, the
// same way they always have.
type Wallet struct {
	Name     string
	Kind     string // "regular" | "mweb" | "universal" | "watch"
	registry *WalletRegistry
	handle   *oyoltc.Wallet // nil while unloaded

	// Creation params kept in RAM so Load can reopen the handle without
	// asking the user to re-enter the seed. Set once at first Create*.
	seedString       string   // free-form user seed (all kinds share the same chain shape)
	addressCount     int      // initial P2WPKH pre-allocation pool (regular/universal)
	mwebAddressCount int      // MWEB addresses to re-allocate on Load (mweb/universal)
	birthHeight      int64    // earliest block to scan for MWEB bootstrap (0 = genesis)
	kinds            []string // universal-side cap (subset of {"p2wpkh","mweb"})
	watchAddresses   []string // watch-only — rebuilt at unload from status

	mu     sync.Mutex
	rev    uint64
	cached []byte // last-known status JSON; served while unloaded
	loaded bool
}

// Loaded returns true if the wallet has an open liboyoltc handle.
// /api/wallets renders this so the UI can pick the right buttons
// (Unload vs Load+Delete) and gate sends.
func (w *Wallet) Loaded() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.loaded
}

// snapshotLocked refreshes cached status if liboyoltc's revision moved.
// While unloaded the handle is nil — return the last-known cached JSON
// so /api/wallets can still show the wallet in the list.
func (w *Wallet) snapshotLocked() []byte {
	if w.handle == nil {
		return w.cached
	}
	rev, js, changed, err := w.handle.StatusSince(w.rev)
	if err == nil && changed {
		w.rev = rev
		w.cached = js
	}
	return w.cached
}

func (w *Wallet) StatusJSON() json.RawMessage {
	w.mu.Lock()
	defer w.mu.Unlock()
	return append(json.RawMessage(nil), w.snapshotLocked()...)
}

func (w *Wallet) BalancesJSON() json.RawMessage {
	w.mu.Lock()
	defer w.mu.Unlock()
	var m struct {
		Balances json.RawMessage `json:"balances"`
	}
	_ = json.Unmarshal(w.snapshotLocked(), &m)
	if len(m.Balances) > 0 {
		return m.Balances
	}
	return json.RawMessage(`{"mine":{"trusted":0,"untrusted_pending":0,"immature":0,"available":0}}`)
}

// AddressesJSON returns a frontend-compatible address list. liboyoltc's
// status carries balances in satoshis under per-kind sub-objects; this
// flattens to {address, confirmed (LTC), pending_in (LTC), pending_out (LTC)}
// regardless of wallet kind so the same renderer works for node, OYO
// regular, and OYO MWEB wallets.
func (w *Wallet) AddressesJSON() json.RawMessage {
	w.mu.Lock()
	defer w.mu.Unlock()

	type rawRegular struct {
		Index         int    `json:"index"`
		Kind          string `json:"kind"`
		Address       string `json:"address"`
		ScriptPubKey  string `json:"script_pubkey"`
		ConfirmedSat  int64  `json:"confirmed_sat"`
		PendingInSat  int64  `json:"pending_in_sat"`
		PendingOutSat int64  `json:"pending_out_sat"`
		ImmatureSat   int64  `json:"immature_sat"`
		Desync        bool   `json:"desync"`
	}
	type rawMweb struct {
		Index         int    `json:"index"`
		Address       string `json:"address"`
		ConfirmedSat  int64  `json:"confirmed_sat"`
		PendingInSat  int64  `json:"pending_in_sat"`
		PendingOutSat int64  `json:"pending_out_sat"`
		Desync        bool   `json:"desync"`
	}
	var status struct {
		Addresses []rawRegular    `json:"addresses"`
		Balances  json.RawMessage `json:"balances"`
		Mweb      *struct {
			Addresses []rawMweb `json:"addresses"`
		} `json:"mweb"`
	}
	_ = json.Unmarshal(w.snapshotLocked(), &status)

	// Both LTC float fields (for frontend rendering) and the original *_sat
	// ints (kept for E2E parity tests that read sat-precision numbers).
	// index/kind/script_pubkey/desync preserved verbatim from the C status.
	type entry struct {
		Index         int     `json:"index"`
		Kind          string  `json:"kind"`
		Address       string  `json:"address"`
		ScriptPubKey  string  `json:"script_pubkey,omitempty"`
		Confirmed     float64 `json:"confirmed"`
		PendingIn     float64 `json:"pending_in,omitempty"`
		PendingOut    float64 `json:"pending_out,omitempty"`
		Immature      float64 `json:"immature,omitempty"`
		ConfirmedSat  int64   `json:"confirmed_sat"`
		PendingInSat  int64   `json:"pending_in_sat"`
		PendingOutSat int64   `json:"pending_out_sat"`
		ImmatureSat   int64   `json:"immature_sat,omitempty"`
		Desync        bool    `json:"desync,omitempty"`
	}
	const satToLtc = 1e8

	out := make([]entry, 0)
	for _, a := range status.Addresses {
		out = append(out, entry{
			Index:         a.Index,
			Kind:          a.Kind,
			Address:       a.Address,
			ScriptPubKey:  a.ScriptPubKey,
			Confirmed:     float64(a.ConfirmedSat) / satToLtc,
			PendingIn:     float64(a.PendingInSat) / satToLtc,
			PendingOut:    float64(a.PendingOutSat) / satToLtc,
			Immature:      float64(a.ImmatureSat) / satToLtc,
			ConfirmedSat:  a.ConfirmedSat,
			PendingInSat:  a.PendingInSat,
			PendingOutSat: a.PendingOutSat,
			ImmatureSat:   a.ImmatureSat,
			Desync:        a.Desync,
		})
	}
	if status.Mweb != nil {
		for _, a := range status.Mweb.Addresses {
			out = append(out, entry{
				Index:         a.Index,
				Kind:          "mweb",
				Address:       a.Address,
				Confirmed:     float64(a.ConfirmedSat) / satToLtc,
				PendingIn:     float64(a.PendingInSat) / satToLtc,
				PendingOut:    float64(a.PendingOutSat) / satToLtc,
				ConfirmedSat:  a.ConfirmedSat,
				PendingInSat:  a.PendingInSat,
				PendingOutSat: a.PendingOutSat,
				Desync:        a.Desync,
			})
		}
	}

	var netPending float64
	for _, e := range out {
		netPending += e.PendingIn - e.PendingOut
	}
	body, _ := json.Marshal(map[string]interface{}{
		"addresses":   out,
		"balances":    status.Balances,
		"net_pending": netPending,
	})
	return body
}

// SecretsJSON exports the wallet's secret material for backup / audit:
// the original free-form seed string (the master backup — re-derives
// everything) merged with the engine-derived per-binding WIF keys and
// MWEB scan/spend secrets. DANGEROUS — exposes full spend authority; the
// HTTP layer gates access behind an explicit confirm.
func (w *Wallet) SecretsJSON() (json.RawMessage, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if !w.loaded || w.handle == nil {
		return nil, errUnloadedWallet
	}
	derived, err := w.handle.ExportSecrets()
	if err != nil {
		return nil, err
	}
	merged := map[string]interface{}{}
	_ = json.Unmarshal(derived, &merged)
	merged["name"] = w.Name
	merged["seed"] = w.seedString
	out, err := json.Marshal(merged)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (w *Wallet) UTXOsJSON() json.RawMessage {
	w.mu.Lock()
	defer w.mu.Unlock()
	// Merge canonical (P2WPKH / bech32) UTXOs with MWEB UTXOs into a single
	// list. Canonical entries carry kind="p2wpkh"/"bech32" already; MWEB
	// entries get kind="mweb" added here so the manual-input picker (and
	// any other consumer) can distinguish without parsing two arrays.
	var m struct {
		UTXOs []map[string]interface{} `json:"utxos"`
		MWEB  struct {
			UTXOs []map[string]interface{} `json:"utxos"`
		} `json:"mweb"`
	}
	_ = json.Unmarshal(w.snapshotLocked(), &m)
	combined := make([]map[string]interface{}, 0, len(m.UTXOs)+len(m.MWEB.UTXOs))
	combined = append(combined, m.UTXOs...)
	for _, u := range m.MWEB.UTXOs {
		if _, has := u["kind"]; !has {
			u["kind"] = "mweb"
		}
		combined = append(combined, u)
	}
	out, err := json.Marshal(combined)
	if err != nil || len(combined) == 0 {
		return json.RawMessage("[]")
	}
	return out
}

// HistoryJSON derives a simple tx-history view from the wallet's current
// UTXO set. Each UTXO produces a `receive` event (or `pending_in` when
// the funding tx is still unconfirmed). Each UTXO also flagged
// `spent_pending=true` produces a `pending_out` event pointing at the
// spending mempool tx. Caveat: this is a *visible state* derivation —
// once a UTXO confirms-spent, liboyoltc's status JSON drops it from the
// list, so confirmed sends and fully-spent receives don't appear here.
// A complete on-chain history requires an event-log on the C-side
// (next iteration).
//
// `maxCount` caps the returned list (matching the listtransactions
// shape used by the node-wallet path).
func (w *Wallet) HistoryJSON(maxCount int) json.RawMessage {
	w.mu.Lock()
	defer w.mu.Unlock()

	type rawUtxo struct {
		Txid             string `json:"txid"`
		Vout             uint32 `json:"vout"`
		AmountSat        int64  `json:"amount_sat"`
		Height           int64  `json:"height"`
		Confirmed        bool   `json:"confirmed"`
		SpentPending     bool   `json:"spent_pending"`
		SpentPendingTxid string `json:"spent_pending_txid"`
		Kind             string `json:"kind"`
	}
	type rawMwebUtxo struct {
		Commitment string `json:"commitment"`
		AmountSat  int64  `json:"amount_sat"`
		Height     int64  `json:"height"`
		Confirmed  bool   `json:"confirmed"`
	}
	var status struct {
		TipHeight int64     `json:"tip_height"`
		UTXOs     []rawUtxo `json:"utxos"`
		Mweb      *struct {
			UTXOs []rawMwebUtxo `json:"utxos"`
		} `json:"mweb"`
	}
	_ = json.Unmarshal(w.snapshotLocked(), &status)

	type entry struct {
		Txid          string  `json:"txid"`
		Category      string  `json:"category"`
		Amount        float64 `json:"amount"`
		AmountSat     int64   `json:"amount_sat"`
		Confirmations int64   `json:"confirmations"`
		Height        int64   `json:"height,omitempty"`
		Kind          string  `json:"kind,omitempty"`
	}
	const satToLtc = 1e8
	confs := func(height int64, confirmed bool) int64 {
		if !confirmed || height <= 0 || status.TipHeight < 0 {
			return 0
		}
		c := status.TipHeight - height + 1
		if c < 0 {
			c = 0
		}
		return c
	}
	out := make([]entry, 0, len(status.UTXOs))
	for _, u := range status.UTXOs {
		cat := "receive"
		if !u.Confirmed {
			cat = "pending_in"
		}
		out = append(out, entry{
			Txid:          u.Txid,
			Category:      cat,
			Amount:        float64(u.AmountSat) / satToLtc,
			AmountSat:     u.AmountSat,
			Confirmations: confs(u.Height, u.Confirmed),
			Height:        u.Height,
			Kind:          u.Kind,
		})
		if u.SpentPending && u.SpentPendingTxid != "" {
			out = append(out, entry{
				Txid:          u.SpentPendingTxid,
				Category:      "pending_out",
				Amount:        -float64(u.AmountSat) / satToLtc,
				AmountSat:     -u.AmountSat,
				Confirmations: 0,
				Kind:          u.Kind,
			})
		}
	}
	if status.Mweb != nil {
		for _, u := range status.Mweb.UTXOs {
			cat := "receive"
			if !u.Confirmed {
				cat = "pending_in"
			}
			out = append(out, entry{
				// MWEB has no txid for outputs (kernel-based); the
				// commitment is the natural identity. We expose it as
				// `txid` so the existing frontend (which links by
				// txid) at least renders a copy-able id; explorer-tx
				// link won't resolve for MWEB outputs but that's
				// already the case for other MWEB views.
				Txid:          u.Commitment,
				Category:      cat,
				Amount:        float64(u.AmountSat) / satToLtc,
				AmountSat:     u.AmountSat,
				Confirmations: confs(u.Height, u.Confirmed),
				Height:        u.Height,
				Kind:          "mweb",
			})
		}
	}
	// Pending entries first (height==0 / unconfirmed sort to the top),
	// then by height desc so the latest activity is visible without
	// scrolling. Frontend additionally `arr.slice().reverse()` — we
	// pre-sort newest-first and the reverse there will swap, so emit
	// oldest-first to compensate. (Matches how listtransactions feeds
	// the same renderer.)
	sort.SliceStable(out, func(i, j int) bool {
		// Treat unconfirmed (Confirmations==0 + height==0) as newest.
		ai, aj := out[i].Height, out[j].Height
		if ai == 0 && aj != 0 {
			return false
		}
		if aj == 0 && ai != 0 {
			return true
		}
		return ai < aj
	})
	if maxCount > 0 && len(out) > maxCount {
		out = out[len(out)-maxCount:]
	}
	body, _ := json.Marshal(out)
	return body
}

func (w *Wallet) AddressJSON(index int) (json.RawMessage, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	var m struct {
		Addresses []struct {
			Index   int    `json:"index"`
			Address string `json:"address"`
		} `json:"addresses"`
	}
	if err := json.Unmarshal(w.snapshotLocked(), &m); err != nil {
		return nil, err
	}
	for _, a := range m.Addresses {
		if a.Index == index {
			return json.Marshal(map[string]string{"address": a.Address})
		}
	}
	return nil, fmt.Errorf("address index out of range")
}

// WalletOp adapts the canonical-rescan / sync op to the liboyoltc op pump.
// Sync = chain-level forward sync (getblockchaininfo + block walk).
type WalletOp struct {
	wallet *Wallet
	open   func() (*oyoltc.Op, error)
}

// RescanOp wraps oyo_wallet_rescan (canonical-side scantxoutset) for
// runWalletOp. Distinct from BootstrapOp() which targets the MWEB
// side via oyo_wallet_bootstrap.
func (w *Wallet) RescanOp() *WalletOp {
	return &WalletOp{wallet: w, open: func() (*oyoltc.Op, error) {
		h, err := w.liveHandle()
		if err != nil {
			return nil, err
		}
		return h.Rescan()
	}}
}

func (w *Wallet) SyncOp() *WalletOp {
	return &WalletOp{wallet: w, open: func() (*oyoltc.Op, error) { return w.registry.chain.Sync() }}
}

func runWalletOp(rpc *RPCClient, op *WalletOp) (json.RawMessage, error) {
	o, err := op.open()
	if err != nil {
		return nil, err
	}
	defer o.Close()
	return oyoltc.RunOp(o, rpc.CallJSON)
}
