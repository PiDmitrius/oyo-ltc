package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"oyo-web/oyoltc"
)

// isStealthAddress returns true when addr looks like an MWEB stealth bech32
// (hrp ends in "mweb"). Used only for UX hints (e.g. error messages) —
// the actual routing decision is made inside liboyoltc's oyo_wallet_send.
func isStealthAddress(addr string) bool {
	sep := strings.IndexByte(addr, '1')
	if sep <= 0 {
		return false
	}
	hrp := strings.ToLower(addr[:sep])
	return strings.HasSuffix(hrp, "mweb")
}

// API handles HTTP endpoints.
type API struct {
	rpc          *RPCClient
	mempool      *MempoolWatcher
	wallets      *WalletRegistry
	confirms     *confirmCache
	queue        *QueueRegistry
	workdir      string // persistent state dir (chain mirror, ...)
	prefix       string // public-facing URL prefix (e.g. "/oyo"); empty when none
	network      string // chain as configured: "main"/"mainnet" | "test"/... | "regtest"
	oyoSupported bool
	oyoVersion   int
	oyoFeatures  []string
}

func (a *API) hasOyoFeature(name string) bool {
	if a == nil || !a.oyoSupported {
		return false
	}
	for _, f := range a.oyoFeatures {
		if f == name {
			return true
		}
	}
	return false
}

// confirmCache holds signed (but not yet broadcast) tx hexes between
// /api/wallet/estimate-send and a follow-up /api/wallet/send?confirm_token.
// Key is the txid of the signed tx — the same value the caller sees back
// from estimate-send. TTL bounds how long a user can sit on the modal
// before the tx must be re-estimated. GC sweeps expired entries.
type confirmCacheEntry struct {
	txHex     string
	wallet    string
	expiresAt time.Time
}
type confirmCache struct {
	mu      sync.Mutex
	entries map[string]confirmCacheEntry
}

const confirmCacheTTL = 5 * time.Minute

func newConfirmCache() *confirmCache {
	return &confirmCache{entries: make(map[string]confirmCacheEntry)}
}

func (c *confirmCache) put(txid, txHex, wallet string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.entries[txid] = confirmCacheEntry{
		txHex:     txHex,
		wallet:    wallet,
		expiresAt: time.Now().Add(confirmCacheTTL),
	}
}

// take removes and returns the cached entry. Returns ok=false if the
// token is unknown OR expired (treated identically — the modal must
// re-estimate either way).
func (c *confirmCache) take(txid string) (confirmCacheEntry, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.entries[txid]
	if !ok {
		return confirmCacheEntry{}, false
	}
	delete(c.entries, txid)
	if time.Now().After(e.expiresAt) {
		return confirmCacheEntry{}, false
	}
	return e, true
}

func (c *confirmCache) gc() {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now()
	for k, e := range c.entries {
		if now.After(e.expiresAt) {
			delete(c.entries, k)
		}
	}
}

// NewAPI creates a new API handler. network selects the chain context for
// liboyoltc (regtest / test / main); empty defaults to regtest. trackMempool
// turns on the mempool sync tailer. workdir is forwarded to liboyoltc so
// the persistent chain mirror lives under {workdir}; empty workdir keeps
// mirror state volatile for dev/test mode.
func NewAPI(rpc *RPCClient, network, workdir, prefix string, trackMempool bool, rollbackWindow int, nativeBlockParse bool) (*API, error) {
	reg, err := NewWalletRegistry(rpc, network, workdir, trackMempool, rollbackWindow, nativeBlockParse)
	if err != nil {
		return nil, err
	}
	a := &API{rpc: rpc, wallets: reg, workdir: workdir, prefix: prefix, network: network, confirms: newConfirmCache()}
	a.queue = newQueueRegistry(a)
	return a, nil
}

// Close releases liboyoltc resources owned by the API.
func (a *API) Close() error {
	if a.queue != nil {
		a.queue.Stop()
	}
	if a.wallets != nil {
		return a.wallets.Close()
	}
	return nil
}

// Init detects OYO support, loads all wallets and starts the OYO chain
// tailer (background chain.Sync ticker that keeps OYO-wallet UTXO sets
// current without requiring user-driven rescans).
func (a *API) Init() {
	if a.wallets != nil {
		a.wallets.StartTailer(3 * time.Second)
	}
	// Confirm-cache GC: sweep expired entries every minute. Cheap; entries
	// already auto-clear on take(), this just bounds memory if a user
	// abandons the confirm modal mid-flow.
	if a.confirms != nil {
		go func() {
			t := time.NewTicker(time.Minute)
			defer t.Stop()
			for range t.C {
				a.confirms.gc()
			}
		}()
	}
	// Detect OYO extensions
	result, err := a.rpc.CallRaw("oyo-version", nil)
	if err == nil {
		var v struct {
			Version  int      `json:"version"`
			Features []string `json:"features"`
		}
		if json.Unmarshal(result, &v) == nil {
			a.oyoSupported = true
			a.oyoVersion = v.Version
			a.oyoFeatures = v.Features
			log.Printf("OYO extensions detected: v%d, features: %v", v.Version, v.Features)
		}
	} else {
		log.Printf("OYO extensions not available: %v", err)
	}

	// Load all wallets from disk
	a.loadAllWallets()
}

// loadAllWallets discovers and loads all wallets.
func (a *API) loadAllWallets() {
	dirRaw, err := a.rpc.CallRaw("listwalletdir", nil)
	if err != nil {
		log.Printf("Warning: listwalletdir failed: %v", err)
		return
	}
	var dir struct {
		Wallets []struct {
			Name string `json:"name"`
		} `json:"wallets"`
	}
	if err := json.Unmarshal(dirRaw, &dir); err != nil {
		return
	}

	// Get already loaded wallets
	loadedRaw, _ := a.rpc.CallRaw("listwallets", nil)
	var loaded []string
	json.Unmarshal(loadedRaw, &loaded)
	loadedMap := make(map[string]bool)
	for _, w := range loaded {
		loadedMap[w] = true
	}

	for _, w := range dir.Wallets {
		if !loadedMap[w.Name] {
			_, err := a.rpc.CallRaw("loadwallet", []interface{}{w.Name})
			if err != nil {
				log.Printf("Warning: could not load wallet %q: %v", w.Name, err)
			} else {
				log.Printf("Loaded wallet %q", w.Name)
			}
		}
	}
}

// Register registers all API routes.
func (a *API) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/info", a.handleInfo)
	mux.HandleFunc("/api/oyo", a.handleOyo)

	// API spec (consumed by /api-docs/ Swagger UI + external tooling)
	mux.HandleFunc("/api/openapi.yaml", a.handleOpenAPI)

	// Search
	mux.HandleFunc("/api/search", a.handleSearch)

	// Blockchain
	mux.HandleFunc("/api/blocks", a.handleBlocks)
	mux.HandleFunc("/api/block/", a.handleBlock)
	mux.HandleFunc("/api/tx/", a.handleTx)
	mux.HandleFunc("/api/address/", a.handleAddress)
	mux.HandleFunc("/api/mempool", a.handleMempool)
	mux.HandleFunc("/api/fees", a.handleFees)
	mux.HandleFunc("/api/peers", a.handlePeers)

	// Wallet management
	mux.HandleFunc("/api/wallets", a.handleWallets)
	mux.HandleFunc("/api/wallet/create", a.handleWalletCreate)
	mux.HandleFunc("/api/wallet/load", a.handleWalletLoad)
	mux.HandleFunc("/api/wallet/unload", a.handleWalletUnload)
	mux.HandleFunc("/api/wallet/delete", a.handleWalletDeleteFromDisk)
	mux.HandleFunc("/api/wallet/info", a.handleWalletInfo)
	mux.HandleFunc("/api/wallet/balances", a.handleWalletBalances)
	mux.HandleFunc("/api/wallet/addresses", a.handleWalletAddresses)
	mux.HandleFunc("/api/wallet/newaddress", a.handleWalletNewAddress)
	mux.HandleFunc("/api/wallet/history", a.handleWalletHistory)
	mux.HandleFunc("/api/wallet/send", a.handleWalletSend)
	mux.HandleFunc("/api/wallet/estimate-send", a.handleWalletEstimateSend)
	mux.HandleFunc("/api/wallet/utxos", a.handleWalletUTXOs)
	mux.HandleFunc("/api/wallet/secrets", a.handleWalletSecrets)
	mux.HandleFunc("/api/wallet/export", a.handleWalletExport)
	mux.HandleFunc("/api/wallet/import", a.handleWalletImport)
	mux.HandleFunc("/api/wallet/rescan", a.handleWalletRescan)
	mux.HandleFunc("/api/chain/sync", a.handleChainSync)
	mux.HandleFunc("/api/chain/mempool-sync", a.handleChainMempoolSync)
	mux.HandleFunc("/api/wallet/shadow", a.handleWalletShadow)
	mux.HandleFunc("/api/wallet/shadow/refresh", a.handleWalletShadowRefresh)

	// Sync / state inspection
	mux.HandleFunc("/api/syncing", a.handleSyncing)
	mux.HandleFunc("/api/syncing/resync", a.handleResync)

	// Queue — deferred / scheduled sends
	mux.HandleFunc("/api/queue", a.handleQueue)
	mux.HandleFunc("/api/queue/", a.handleQueueItem)

	// Regtest
	mux.HandleFunc("/api/mine", a.handleMine)
}

func (a *API) jsonResponse(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(data)
}

func (a *API) jsonError(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// requireWallet extracts wallet name from query param, returns empty string + error response if missing.
func (a *API) requireWallet(w http.ResponseWriter, r *http.Request) string {
	name := r.URL.Query().Get("name")
	if name == "" {
		a.jsonError(w, "wallet name required", 400)
		return ""
	}
	return name
}

// errUnloaded is returned by Wallet methods that need a live
// liboyoltc handle when the wallet is currently unloaded. Surfaced as
// 409 by the API layer; the user-facing remediation is /api/wallet/load.
var errUnloadedWallet = errors.New("wallet is unloaded — load it first via /api/wallet/load")

// GET /api/info -- blockchain + network info
func (a *API) handleInfo(w http.ResponseWriter, r *http.Request) {
	chain, err := a.rpc.CallRaw("getblockchaininfo", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	net, err := a.rpc.CallRaw("getnetworkinfo", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	mempool, err := a.rpc.CallRaw("getmempoolinfo", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	mining, err := a.rpc.CallRaw("getmininginfo", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}

	resp := map[string]json.RawMessage{
		"blockchain": chain,
		"network":    net,
		"mempool":    mempool,
		"mining":     mining,
	}
	// engine: liboyoltc chain status (tip_height = local mirror/engine height)
	// so the UI can show engine-vs-node sync progress. Best-effort.
	if a.wallets != nil {
		if eng, e := a.wallets.ChainStatusJSON(); e == nil {
			resp["engine"] = eng
		}
	}
	a.jsonResponse(w, resp)
}

// GET /api/openapi.yaml — embedded spec with a request-driven
// `servers:` URL so Swagger UI's "Try it out" calls land at the
// right host even behind a reverse-proxy prefix.
//
// The base URL is reconstructed from forward headers (set by any
// reasonable proxy: nginx/caddy/traefik/...) plus the prefix
// configured on this oyo-web. Falls back to the raw Host when
// X-Forwarded-Host isn't present so the local dev case (`oyo-web`
// listening directly on :8881) keeps working.
func (a *API) handleOpenAPI(w http.ResponseWriter, r *http.Request) {
	scheme := r.Header.Get("X-Forwarded-Proto")
	if scheme == "" {
		if r.TLS != nil {
			scheme = "https"
		} else {
			scheme = "http"
		}
	}
	host := r.Header.Get("X-Forwarded-Host")
	if host == "" {
		host = r.Host
	}
	base := scheme + "://" + host + a.prefix

	body := injectServerURL(openapiYAML, base)
	w.Header().Set("Content-Type", "application/yaml")
	w.Header().Set("Cache-Control", "no-cache")
	_, _ = w.Write(body)
}

// injectServerURL replaces the `servers:` block in the YAML body
// with a single dynamic entry pointing at `base`. The block runs
// from the literal `servers:` line through the next top-level YAML
// key (a non-indented line starting with letter or `#`). Bytes-only
// rewrite — no YAML parser dependency.
func injectServerURL(spec []byte, base string) []byte {
	src := string(spec)
	startMarker := "\nservers:"
	idx := strings.Index(src, startMarker)
	if idx < 0 && strings.HasPrefix(src, "servers:") {
		idx = -1 // matched at file start
	}
	var blockStart int
	if idx < 0 {
		blockStart = 0
	} else {
		blockStart = idx + 1 // skip the leading newline
	}
	// Find the end: first line after `servers:` that starts at column 0
	// with a letter or `#` (= next top-level key or comment block).
	rest := src[blockStart:]
	endRel := -1
	pos := 0
	for {
		nl := strings.IndexByte(rest[pos:], '\n')
		if nl < 0 {
			break
		}
		lineStart := pos + nl + 1
		if lineStart >= len(rest) {
			break
		}
		// Skip the very first line (the `servers:` line itself).
		if pos == 0 {
			pos = lineStart
			continue
		}
		c := rest[lineStart]
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '#' {
			endRel = lineStart
			break
		}
		pos = lineStart
	}
	if endRel < 0 {
		// Couldn't find a block end — bail out, return unmodified.
		return spec
	}
	replacement := "servers:\n  - url: " + base + "\n    description: this server\n\n"
	out := src[:blockStart] + replacement + rest[endRel:]
	return []byte(out)
}

// GET /api/oyo -- OYO extension info
func (a *API) handleOyo(w http.ResponseWriter, r *http.Request) {
	features := a.oyoFeatures
	if features == nil {
		features = []string{}
	}
	a.jsonResponse(w, map[string]interface{}{
		"oyo":      a.oyoSupported,
		"version":  a.oyoVersion,
		"features": features,
	})
}

// GET /api/search?q=... -- smart lookup
func (a *API) handleSearch(w http.ResponseWriter, r *http.Request) {
	q := strings.TrimSpace(r.URL.Query().Get("q"))
	if q == "" {
		a.jsonError(w, "query required", 400)
		return
	}

	// Pure digits -> block height
	if height, err := strconv.Atoi(q); err == nil {
		hashRaw, err := a.rpc.CallRaw("getblockhash", []interface{}{height})
		if err != nil {
			a.jsonError(w, err.Error(), 404)
			return
		}
		var hash string
		json.Unmarshal(hashRaw, &hash)
		block, err := a.rpc.CallRaw("getblock", []interface{}{hash, 2})
		if err != nil {
			a.jsonError(w, err.Error(), 404)
			return
		}
		a.jsonResponse(w, map[string]interface{}{"type": "block", "data": json.RawMessage(block)})
		return
	}

	// 64 hex chars -> txid or block hash
	if len(q) == 64 && isHex(q) {
		// Try transaction first (getrawtransaction, then wallet fallback for MWEB)
		tx, err := a.rpc.CallRaw("getrawtransaction", []interface{}{q, true})
		if err == nil {
			a.jsonResponse(w, map[string]interface{}{"type": "tx", "data": json.RawMessage(a.enrichTxVin(tx))})
			return
		}
		if walletTx := a.findTxInWallets(q); walletTx != nil {
			a.jsonResponse(w, map[string]interface{}{"type": "tx", "data": walletTx})
			return
		}
		// Try block
		block, err := a.rpc.CallRaw("getblock", []interface{}{q, 2})
		if err == nil {
			a.jsonResponse(w, map[string]interface{}{"type": "block", "data": json.RawMessage(block)})
			return
		}
		a.jsonError(w, "not found", 404)
		return
	}

	// Otherwise treat as address
	data := a.lookupAddress(q, false, false, 0)
	if data == nil {
		a.jsonError(w, "invalid address or query", 400)
		return
	}
	a.jsonResponse(w, data)
}

// GET /api/address/<addr>
func (a *API) handleAddress(w http.ResponseWriter, r *http.Request) {
	addr := strings.TrimPrefix(r.URL.Path, "/api/address/")
	if addr == "" {
		a.jsonError(w, "address required", 400)
		return
	}
	// ?verify=node cross-checks the mirror-derived balance against the node's
	// scantxoutset (a self-audit aid used by the e2e tests). Off by default so
	// the hot path stays node-query-free.
	verifyNode := r.URL.Query().Get("verify") == "node"
	// ?history=1 adds the full received-output history (spent + unspent) from
	// the regular journal. Opt-in so the default lookup / search stays light.
	history := r.URL.Query().Get("history") == "1"
	// ?limit=N widens the explorer scan window (the UI's doubling "load more");
	// the engine clamps it to its ceiling. Absent → engine default window.
	limit := 0
	if v := r.URL.Query().Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			limit = n
		}
	}
	data := a.lookupAddress(addr, verifyNode, history, limit)
	if data == nil {
		a.jsonError(w, "invalid address", 400)
		return
	}
	a.jsonResponse(w, data)
}

// shapeMwebAddressStatus turns the engine's MWEB arm of oyo_chain_address_status
// into the explorer's address shape. MWEB outputs have no txid (stealth) —
// output_id stands in. owned/wallet/foreign and the sums come straight from the
// engine (the chain's keychain-derived MwebAddress), so the Go layer no longer
// iterates wallets for stealth history.
func shapeMwebAddressStatus(raw json.RawMessage, history bool) map[string]interface{} {
	var st struct {
		TipHeight         int64  `json:"tip_height"`
		ConfirmedSat      int64  `json:"confirmed_sat"`
		PendingInSat      int64  `json:"pending_in_sat"`
		PendingOutSat     int64  `json:"pending_out_sat"`
		Owned             bool   `json:"owned"`
		Foreign           bool   `json:"foreign"`
		Wallet            string `json:"wallet"`
		TotalReceivedSat  int64  `json:"total_received_sat"`
		TotalSpentSat     int64  `json:"total_spent_sat"`
		ReceivedCount     int64  `json:"received_count"`
		ReceivedTruncated bool   `json:"received_truncated"`
		Received          []struct {
			OutputID    string `json:"output_id"`
			AmountSat   int64  `json:"amount_sat"`
			Height      int64  `json:"height"`
			Spent       bool   `json:"spent"`
			SpentHeight int64  `json:"spent_height"`
		} `json:"received"`
	}
	if json.Unmarshal(raw, &st) != nil {
		return nil
	}
	out := map[string]interface{}{
		"mweb":            true,
		"balance":         float64(st.ConfirmedSat) / 1e8,
		"balance_sat":     st.ConfirmedSat,
		"confirmed_sat":   st.ConfirmedSat,
		"tip_height":      st.TipHeight,
		"pending_in_sat":  st.PendingInSat,
		"pending_out_sat": st.PendingOutSat,
	}
	if st.Foreign {
		out["foreign"] = true
	}
	if st.Owned {
		out["owned"] = true
	}
	if st.Wallet != "" {
		out["wallet"] = st.Wallet
	}
	if history {
		recv := make([]map[string]interface{}, 0, len(st.Received))
		for _, o := range st.Received {
			recv = append(recv, map[string]interface{}{
				"output_id":    o.OutputID,
				"amount":       float64(o.AmountSat) / 1e8,
				"amount_sat":   o.AmountSat,
				"height":       o.Height,
				"spent":        o.Spent,
				"spent_height": o.SpentHeight,
			})
		}
		out["received"] = recv
		out["total_received_sat"] = st.TotalReceivedSat
		out["total_spent_sat"] = st.TotalSpentSat
		out["received_count"] = st.ReceivedCount
		out["received_truncated"] = st.ReceivedTruncated
	}
	return out
}

// mirrorAddressStatus pulls the confirmed balance, immature split and unspent
// set for a canonical address from liboyoltc's local regular-UTXO mirror (zero
// node queries); MWEB stealth addresses are dispatched to shapeMwebAddressStatus.
// Returns nil if the engine is unavailable or the address is non-canonical
// (caller falls back / leaves the field unset).
func (a *API) mirrorAddressStatus(address string, history bool, limit int) map[string]interface{} {
	raw, err := a.wallets.AddressStatusJSON(address, history, limit)
	if err != nil || raw == nil {
		return nil
	}
	// The engine serves MWEB stealth addresses through this same entrypoint,
	// flagged mweb:true. Shape that branch separately — MWEB outputs carry
	// output_id, not a canonical txid/vout.
	var probe struct {
		Mweb bool `json:"mweb"`
	}
	json.Unmarshal(raw, &probe)
	if probe.Mweb {
		return shapeMwebAddressStatus(raw, history)
	}
	var st struct {
		TipHeight      int64 `json:"tip_height"`
		ConfirmedSat   int64 `json:"confirmed_sat"`
		ImmatureSat    int64 `json:"immature_sat"`
		AvailableSat   int64 `json:"available_sat"`
		UtxoCount      int64 `json:"utxo_count"`
		UtxosTruncated bool  `json:"utxos_truncated"`
		ScanLimit      int64 `json:"scan_limit"`
		NextLimit      int64 `json:"next_limit"`
		Utxos          []struct {
			Txid      string `json:"txid"`
			Vout      uint32 `json:"vout"`
			AmountSat int64  `json:"amount_sat"`
			Height    int64  `json:"height"`
			Coinbase  bool   `json:"coinbase"`
			Pegout    bool   `json:"pegout"`
			Mature    bool   `json:"mature"`
		} `json:"utxos"`
		PendingInSat      int64    `json:"pending_in_sat"`
		PendingOutSat     int64    `json:"pending_out_sat"`
		PendingTxids      []string `json:"pending_txids"`
		TotalReceivedSat  int64    `json:"total_received_sat"`
		TotalSpentSat     int64    `json:"total_spent_sat"`
		ReceivedCount     int64    `json:"received_count"`
		ReceivedTruncated bool     `json:"received_truncated"`
		Received          []struct {
			Txid        string `json:"txid"`
			Vout        uint32 `json:"vout"`
			AmountSat   int64  `json:"amount_sat"`
			Height      int64  `json:"height"`
			Spent       bool   `json:"spent"`
			SpentHeight int64  `json:"spent_height"`
			Coinbase    bool   `json:"coinbase"`
			Pegout      bool   `json:"pegout"`
		} `json:"received"`
	}
	if json.Unmarshal(raw, &st) != nil {
		return nil
	}
	utxos := make([]map[string]interface{}, 0, len(st.Utxos))
	for _, u := range st.Utxos {
		utxos = append(utxos, map[string]interface{}{
			"txid":       u.Txid,
			"vout":       u.Vout,
			"amount":     float64(u.AmountSat) / 1e8,
			"amount_sat": u.AmountSat,
			"height":     u.Height,
			"coinbase":   u.Coinbase,
			"pegout":     u.Pegout,
			"mature":     u.Mature,
		})
	}
	out := map[string]interface{}{
		"balance":         float64(st.ConfirmedSat) / 1e8,
		"balance_sat":     st.ConfirmedSat,
		"confirmed_sat":   st.ConfirmedSat,
		"tip_height":      st.TipHeight,
		"immature_sat":    st.ImmatureSat,
		"available_sat":   st.AvailableSat,
		"pending_in_sat":  st.PendingInSat,
		"pending_out_sat": st.PendingOutSat,
		"utxo_count":      st.UtxoCount,
		"utxos_truncated": st.UtxosTruncated,
		"scan_limit":      st.ScanLimit,
		"next_limit":      st.NextLimit,
		"utxos":           utxos,
	}
	// Unconfirmed txs touching this address, from the engine's mempool index.
	// The aggregate amounts are pending_in/out_sat above; list the txids so the
	// explorer can link to each pending tx.
	if len(st.PendingTxids) > 0 {
		pending := make([]map[string]interface{}, 0, len(st.PendingTxids))
		for _, txid := range st.PendingTxids {
			pending = append(pending, map[string]interface{}{"txid": txid})
		}
		out["pending"] = pending
	}
	if history {
		recv := make([]map[string]interface{}, 0, len(st.Received))
		for _, o := range st.Received {
			recv = append(recv, map[string]interface{}{
				"txid": o.Txid, "vout": o.Vout,
				"amount": float64(o.AmountSat) / 1e8, "amount_sat": o.AmountSat,
				"height": o.Height, "spent": o.Spent, "spent_height": o.SpentHeight,
				"coinbase": o.Coinbase, "pegout": o.Pegout,
			})
		}
		out["received"] = recv
		out["total_received_sat"] = st.TotalReceivedSat
		out["total_spent_sat"] = st.TotalSpentSat
		out["received_count"] = st.ReceivedCount
		out["received_truncated"] = st.ReceivedTruncated
	}
	return out
}

func (a *API) lookupAddress(address string, verifyNode, history bool, limit int) map[string]interface{} {
	validRaw, err := a.rpc.CallRaw("validateaddress", []interface{}{address})
	if err != nil {
		return nil
	}
	var valid struct {
		IsValid bool `json:"isvalid"`
	}
	json.Unmarshal(validRaw, &valid)
	if !valid.IsValid {
		return nil
	}

	result := map[string]interface{}{
		"type":    "address",
		"address": address,
	}

	// MWEB stealth addresses live in a separate UTXO set with no on-chain
	// script index — scantxoutset only knows the canonical side. They are
	// served by the same engine entrypoint as canonical addresses: the chain
	// resolves the address in its keychain-derived MwebAddress set (owned →
	// balance + ?history journal; unknown → foreign/opaque, since we can't
	// decrypt without the owner's scan_secret). `mweb: true, foreign: true`
	// lets the frontend explain rather than silently show "0 LTC".
	if isStealthAddress(address) {
		result["mweb"] = true
		if st := a.mirrorAddressStatus(address, history, limit); st != nil {
			for k, v := range st {
				result[k] = v
			}
			return result
		}
		result["balance"] = 0.0
		result["balance_sat"] = int64(0)
		result["foreign"] = true
		return result
	}

	// Canonical address. The confirmed balance, the immature split and the
	// unspent set come from liboyoltc's local regular-UTXO mirror, which holds
	// every canonical UTXO — so this works for any address (owned or not) with
	// zero node queries. It replaces scantxoutset, which rescans the whole
	// UTXO set, serialises behind wallet rescans and hung the address page in
	// the field. scantxoutset survives only as a degraded-mode fallback.
	mirrorOK := false
	if a.wallets != nil {
		if st := a.mirrorAddressStatus(address, history, limit); st != nil {
			for k, v := range st {
				result[k] = v
			}
			mirrorOK = true
		}
	}
	if !mirrorOK {
		utxoRaw, err := a.rpc.CallRaw("scantxoutset", []interface{}{"start", []interface{}{"addr(" + address + ")"}})
		if err == nil {
			var utxo struct {
				TotalAmount float64           `json:"total_amount"`
				Unspents    []json.RawMessage `json:"unspents"`
			}
			if json.Unmarshal(utxoRaw, &utxo) == nil {
				result["balance"] = utxo.TotalAmount
				result["utxos"] = utxo.Unspents
			}
		}
	}

	// Owned by one of our external wallets? Annotate ownership only — the
	// balance, immature split and pending all come from the engine above now,
	// uniform for owned and foreign addresses (no node mempool scan).
	if a.wallets != nil {
		type ownedAddr struct {
			Kind    string `json:"kind"`
			Address string `json:"address"`
		}
		found := false
		for _, ext := range a.wallets.List() {
			var parsed struct {
				Addresses []ownedAddr `json:"addresses"`
			}
			if json.Unmarshal(ext.AddressesJSON(), &parsed) != nil {
				continue
			}
			for _, ad := range parsed.Addresses {
				if ad.Kind != "mweb" && ad.Address == address {
					result["owned"] = true
					result["wallet"] = ext.Name
					found = true
					break
				}
			}
			if found {
				break
			}
		}
	}

	walletsRaw, _ := a.rpc.CallRaw("listwallets", nil)
	var wallets []string
	json.Unmarshal(walletsRaw, &wallets)

	for _, wName := range wallets {
		info, err := a.rpc.CallWallet(wName, "getaddressinfo", []interface{}{address})
		if err == nil {
			var ai struct {
				IsMine bool `json:"ismine"`
			}
			json.Unmarshal(info, &ai)
			if ai.IsMine {
				result["wallet"] = wName
				break
			}
		}
	}

	// ?verify=node self-audit: compare the mirror's confirmed total against the
	// node's scantxoutset. node_match==true means the local mirror agrees with
	// the node's UTXO set for this address — the cross-check the e2e tests use.
	if verifyNode {
		nodeSat := int64(-1)
		if utxoRaw, e := a.rpc.CallRaw("scantxoutset", []interface{}{"start", []interface{}{"addr(" + address + ")"}}); e == nil {
			var u struct {
				TotalAmount float64 `json:"total_amount"`
			}
			if json.Unmarshal(utxoRaw, &u) == nil {
				nodeSat = int64(math.Round(u.TotalAmount * 1e8))
			}
		}
		result["node_balance_sat"] = nodeSat
		if cs, ok := result["confirmed_sat"].(int64); ok {
			result["node_match"] = nodeSat == cs
		}
	}

	return result
}

// findTxInWallets tries gettransaction on each loaded wallet (fallback for MWEB txs not in txindex).
func (a *API) findTxInWallets(txid string) map[string]interface{} {
	walletsRaw, err := a.rpc.CallRaw("listwallets", nil)
	if err != nil {
		return nil
	}
	var wallets []string
	json.Unmarshal(walletsRaw, &wallets)

	for _, wName := range wallets {
		result, err := a.rpc.CallWallet(wName, "gettransaction", []interface{}{txid, true})
		if err != nil {
			continue
		}
		var tx map[string]interface{}
		if json.Unmarshal(result, &tx) != nil {
			continue
		}
		// Mark as wallet-sourced and add txid
		tx["txid"] = txid
		tx["wallet"] = wName
		tx["source"] = "wallet"
		return tx
	}
	return nil
}

// enrichTxVin adds address and value to each vin from the previous transaction output.
func (a *API) enrichTxVin(txRaw json.RawMessage) json.RawMessage {
	var tx map[string]interface{}
	if err := json.Unmarshal(txRaw, &tx); err != nil {
		return txRaw
	}
	vins, ok := tx["vin"].([]interface{})
	if !ok {
		return txRaw
	}
	for _, v := range vins {
		vin, ok := v.(map[string]interface{})
		if !ok {
			continue
		}
		prevTxid, _ := vin["txid"].(string)
		if prevTxid == "" {
			continue
		}
		prevVout, _ := vin["vout"].(float64)
		prevRaw, err := a.rpc.CallRaw("getrawtransaction", []interface{}{prevTxid, true})
		if err != nil {
			continue
		}
		var prevTx struct {
			Vout []struct {
				Value        float64 `json:"value"`
				N            int     `json:"n"`
				ScriptPubKey struct {
					Addresses []string `json:"addresses"`
					Address   string   `json:"address"`
				} `json:"scriptPubKey"`
			} `json:"vout"`
		}
		if err := json.Unmarshal(prevRaw, &prevTx); err != nil {
			continue
		}
		idx := int(prevVout)
		if idx < len(prevTx.Vout) {
			out := prevTx.Vout[idx]
			vin["value"] = out.Value
			addr := out.ScriptPubKey.Address
			if addr == "" && len(out.ScriptPubKey.Addresses) > 0 {
				addr = out.ScriptPubKey.Addresses[0]
			}
			if addr != "" {
				vin["address"] = addr
			}
		}
	}
	enriched, err := json.Marshal(tx)
	if err != nil {
		return txRaw
	}
	return enriched
}

func isHex(s string) bool {
	for _, c := range s {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return false
		}
	}
	return true
}

// GET /api/blocks?count=10 -- latest blocks
func (a *API) handleBlocks(w http.ResponseWriter, r *http.Request) {
	count := 10
	if c := r.URL.Query().Get("count"); c != "" {
		n, err := strconv.Atoi(c)
		if err != nil || n < 1 {
			a.jsonError(w, "invalid count", 400)
			return
		}
		if n > 100 {
			n = 100
		}
		count = n
	}

	heightRaw, err := a.rpc.CallRaw("getblockcount", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	var height int
	json.Unmarshal(heightRaw, &height)

	blocks := make([]json.RawMessage, 0, count)
	for i := height; i > height-count && i >= 0; i-- {
		hashRaw, err := a.rpc.CallRaw("getblockhash", []interface{}{i})
		if err != nil {
			break
		}
		var hash string
		json.Unmarshal(hashRaw, &hash)
		block, err := a.rpc.CallRaw("getblock", []interface{}{hash})
		if err != nil {
			break
		}
		blocks = append(blocks, block)
	}

	a.jsonResponse(w, map[string]interface{}{
		"height": height,
		"blocks": blocks,
	})
}

// GET /api/block/<hash-or-height>
func (a *API) handleBlock(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/api/block/")
	if id == "" {
		a.jsonError(w, "block hash or height required", 400)
		return
	}

	var hash string
	if n, err := strconv.Atoi(id); err == nil {
		hashRaw, err := a.rpc.CallRaw("getblockhash", []interface{}{n})
		if err != nil {
			a.jsonError(w, err.Error(), 404)
			return
		}
		json.Unmarshal(hashRaw, &hash)
	} else {
		hash = id
	}

	block, err := a.rpc.CallRaw("getblock", []interface{}{hash, 2})
	if err != nil {
		a.jsonError(w, err.Error(), 404)
		return
	}
	a.jsonResponse(w, block)
}

// GET /api/tx/<txid>
func (a *API) handleTx(w http.ResponseWriter, r *http.Request) {
	txid := strings.TrimPrefix(r.URL.Path, "/api/tx/")
	if txid == "" {
		a.jsonError(w, "txid required", 400)
		return
	}
	txRaw, err := a.rpc.CallRaw("getrawtransaction", []interface{}{txid, true})
	if err != nil {
		// Fallback: try gettransaction via wallet endpoints (MWEB txs not in txindex)
		if walletTx := a.findTxInWallets(txid); walletTx != nil {
			a.jsonResponse(w, walletTx)
			return
		}
		a.jsonError(w, err.Error(), 404)
		return
	}
	a.jsonResponse(w, json.RawMessage(a.enrichTxVin(txRaw)))
}

// GET /api/mempool
func (a *API) handleMempool(w http.ResponseWriter, r *http.Request) {
	pool, err := a.rpc.CallRaw("getrawmempool", []interface{}{true})
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	a.jsonResponse(w, pool)
}

// relayFloorSatPerVB returns the node's minimum relay fee as an integer
// sat/vB (>= 1). getmempoolinfo.mempoolminfee is in LTC/kvB. Falls back
// to 1 sat/vB when the node can't be reached or reports nothing.
func (a *API) relayFloorSatPerVB() int64 {
	if raw, err := a.rpc.CallRaw("getmempoolinfo", nil); err == nil {
		var mi struct {
			MempoolMinFee float64 `json:"mempoolminfee"`
		}
		if json.Unmarshal(raw, &mi) == nil && mi.MempoolMinFee > 0 {
			if v := int64(math.Ceil(mi.MempoolMinFee * 1e8 / 1000.0)); v >= 1 {
				return v
			}
		}
	}
	return 1
}

// GET /api/fees — fee-rate presets in sat/vB for the send form.
//
// Maps conf targets 1 / 6 / 24 blocks to high / normal / low through
// estimatesmartfee (ECONOMICAL). When the node has no fee estimate —
// regtest has no fee market, or a fresh mainnet node still warming up —
// the preset falls back to the relay floor. Values are integer sat/vB,
// clamped to be monotonic (low <= normal <= high) so the UI never shows
// an inverted ladder from a noisy estimator.
func (a *API) handleFees(w http.ResponseWriter, r *http.Request) {
	floor := a.relayFloorSatPerVB()
	rate := func(confTarget int) int64 {
		raw, err := a.rpc.CallRaw("estimatesmartfee", []interface{}{confTarget, "ECONOMICAL"})
		if err != nil {
			return floor
		}
		var res struct {
			FeeRate float64 `json:"feerate"` // LTC/kvB
		}
		if json.Unmarshal(raw, &res) != nil || res.FeeRate <= 0 {
			return floor
		}
		v := int64(math.Ceil(res.FeeRate * 1e8 / 1000.0))
		if v < floor {
			v = floor
		}
		return v
	}
	low, normal, high := rate(24), rate(6), rate(1)
	if normal < low {
		normal = low
	}
	if high < normal {
		high = normal
	}
	a.jsonResponse(w, map[string]interface{}{
		"floor":   floor,
		"presets": map[string]int64{"low": low, "normal": normal, "high": high},
	})
}

// GET /api/peers
func (a *API) handlePeers(w http.ResponseWriter, r *http.Request) {
	peers, err := a.rpc.CallRaw("getpeerinfo", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	a.jsonResponse(w, peers)
}

// GET /api/wallets -- list all wallets (loaded + unloaded) with info
func (a *API) handleWallets(w http.ResponseWriter, r *http.Request) {
	// Get loaded wallets
	loadedRaw, err := a.rpc.CallRaw("listwallets", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	var loadedNames []string
	json.Unmarshal(loadedRaw, &loadedNames)
	loadedSet := make(map[string]bool)
	for _, n := range loadedNames {
		loadedSet[n] = true
	}

	// Get all wallets on disk
	dirRaw, _ := a.rpc.CallRaw("listwalletdir", nil)
	var dirResult struct {
		Wallets []struct {
			Name string `json:"name"`
		} `json:"wallets"`
	}
	allNames := loadedNames
	if dirRaw != nil {
		json.Unmarshal(dirRaw, &dirResult)
		seen := make(map[string]bool)
		for _, n := range loadedNames {
			seen[n] = true
		}
		for _, w := range dirResult.Wallets {
			if !seen[w.Name] {
				allNames = append(allNames, w.Name)
			}
		}
	}

	type walletEntry struct {
		Name     string          `json:"name"`
		Loaded   bool            `json:"loaded"`
		Kind     string          `json:"kind"`              // "node" | "oyo"
		Subkind  string          `json:"subkind,omitempty"` // oyo: "regular" | "mweb" | "universal" | "watch" (shadow)
		Desync   bool            `json:"desync,omitempty"`  // oyo-only; surfaces address-level desync
		Info     json.RawMessage `json:"info,omitempty"`
		Balances json.RawMessage `json:"balances,omitempty"`
	}

	entries := make([]walletEntry, 0, len(allNames))
	for _, name := range allNames {
		entry := walletEntry{Name: name, Loaded: loadedSet[name], Kind: "node"}
		if entry.Loaded {
			info, err := a.rpc.CallWallet(name, "getwalletinfo", nil)
			if err == nil {
				entry.Info = info
			}
			bal, err := a.rpc.CallWallet(name, "getbalances", nil)
			if err == nil {
				// Enrich with net pending from MempoolWatcher
				if a.mempool != nil {
					var balMap map[string]interface{}
					if json.Unmarshal(bal, &balMap) == nil {
						// Get wallet addresses from groupings
						groupRaw, gErr := a.rpc.CallWallet(name, "listaddressgroupings", nil)
						if gErr == nil {
							var groups [][][]interface{}
							json.Unmarshal(groupRaw, &groups)
							walletAddrs := make(map[string]bool)
							for _, group := range groups {
								for _, entry := range group {
									if len(entry) >= 1 {
										if addr, ok := entry[0].(string); ok {
											walletAddrs[addr] = true
										}
									}
								}
							}
							// Also include received addresses
							recvRaw, _ := a.rpc.CallWallet(name, "listreceivedbyaddress", []interface{}{0, true, true})
							if recvRaw != nil {
								var recvAddrs []struct {
									Address string `json:"address"`
								}
								json.Unmarshal(recvRaw, &recvAddrs)
								for _, r := range recvAddrs {
									walletAddrs[r.Address] = true
								}
							}
							// Sum mempool pending for wallet addresses
							addrPending := a.mempool.GetAddrPending()
							var netPending float64
							for addr := range walletAddrs {
								if ap, ok := addrPending[addr]; ok {
									netPending += ap.Outputs - ap.Inputs
								}
							}
							if mine, ok := balMap["mine"].(map[string]interface{}); ok {
								if netPending > 0.000000005 || netPending < -0.000000005 {
									mine["net_pending"] = netPending
								}
							}
						}
						// Queue overlay used to live here — disabled for now,
						// the queue is being rebuilt OYO-aware.

						enriched, _ := json.Marshal(balMap)
						bal = enriched
					}
				}
				entry.Balances = bal
			}
		}
		entries = append(entries, entry)
	}
	for _, ext := range a.wallets.List() {
		status := ext.StatusJSON()
		var parsed struct {
			Balances json.RawMessage `json:"balances"`
			Desync   bool            `json:"desync"`
		}
		_ = json.Unmarshal(status, &parsed)
		entry := walletEntry{Name: ext.Name, Loaded: ext.Loaded(), Kind: "oyo", Subkind: ext.Kind, Desync: parsed.Desync}
		entry.Info = status
		// Hide stale balance figures for unloaded wallets — they'd otherwise
		// look "live" until the next sync after Load. UI shows "—" instead.
		if entry.Loaded {
			entry.Balances = parsed.Balances
		}
		entries = append(entries, entry)
	}

	a.jsonResponse(w, entries)
}

// POST /api/wallet/create?name=...&type=seed|regular|mweb|universal
//
// Supported types:
//   - seed:       node HD wallet seeded from a free-form string
//     (deterministic — same seed → same addresses)
//   - regular:    OYO regular wallet (canonical-only)
//   - mweb:       OYO MWEB-only wallet
//   - universal:  OYO universal wallet (canonical + MWEB sides)
//
// Removed (no longer supported):
//   - hd:    creating a node wallet without an explicit seed (non-
//     deterministic — replaced by `seed`)
//   - watch: watch-only wallets (use the explorer for read-only views)
//   - blank: blank wallets used to be the entry point for privkey
//     import; we don't support privkey import anymore
//
// Removed aliases (no longer accepted): external, external_mweb,
// external_universal — collapsed into regular/mweb/universal.
// parseBirthHeight reads the optional wallet birth height from the request.
// 0 / absent means "scan from genesis". A birth height lets the MWEB bootstrap
// skip the pre-birth prefix of the journal (stealth has no per-address on-chain
// index, so the bulk RewindOutput scan is the slow path). The frontend computes
// it from a user-friendly age (~576 blocks/day at 2.5-min blocks) against the
// engine tip; callers may also pass an explicit height.
func parseBirthHeight(r *http.Request) (int64, error) {
	raw := r.URL.Query().Get("birth_height")
	if raw == "" {
		return 0, nil
	}
	bh, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || bh < 0 {
		return 0, errors.New("invalid birth_height")
	}
	return bh, nil
}

func (a *API) handleWalletCreate(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	wtype := r.URL.Query().Get("type")
	if wtype == "" {
		a.jsonError(w, "type required: seed, regular, mweb, or universal", 400)
		return
	}

	switch wtype {
	case "regular":
		seed := r.URL.Query().Get("seed")
		if seed == "" {
			a.jsonError(w, "seed string required", 400)
			return
		}
		count := 8
		if raw := r.URL.Query().Get("address_count"); raw != "" {
			n, nErr := strconv.Atoi(raw)
			if nErr != nil || n <= 0 {
				a.jsonError(w, "invalid address_count", 400)
				return
			}
			count = n
		}
		wallet, err := a.wallets.CreateRegular(name, seed, count)
		if err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		result, err := runWalletOp(a.rpc, wallet.RescanOp())
		if err != nil {
			a.jsonError(w, err.Error(), 502)
			return
		}
		a.jsonResponse(w, map[string]interface{}{"name": name, "type": "oyo_regular", "bootstrap": json.RawMessage(result)})
	case "universal":
		// Universal OYO wallet: both p2wpkh and mweb sides driven by
		// one 32-byte master seed. Optional `kinds` URL param ("p2wpkh"
		// or "mweb") restricts to one side only — useful for receive-only
		// deployments. Default = both kinds.
		seed := r.URL.Query().Get("seed")
		if seed == "" {
			a.jsonError(w, "seed (hex64) required for universal wallet", 400)
			return
		}
		count := 8
		if raw := r.URL.Query().Get("address_count"); raw != "" {
			n, nErr := strconv.Atoi(raw)
			if nErr != nil || n <= 0 {
				a.jsonError(w, "invalid address_count", 400)
				return
			}
			count = n
		}
		var kinds []string
		if raw := r.URL.Query().Get("kinds"); raw != "" {
			for _, k := range strings.Split(raw, ",") {
				k = strings.TrimSpace(k)
				if k != "" {
					kinds = append(kinds, k)
				}
			}
		}
		birthHeight, bhErr := parseBirthHeight(r)
		if bhErr != nil {
			a.jsonError(w, bhErr.Error(), 400)
			return
		}
		wallet, err := a.wallets.CreateUniversal(name, seed, kinds, count, birthHeight)
		if err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		// Pre-allocate MWEB addresses on the same `count` budget the
		// p2wpkh side gets. The C-side open registers the keychain only;
		// the Go layer drives MWEB allocation through NewAddress so
		// wallet.mwebAddressCount tracks them for Unload→Load
		// re-allocation. Index 0 lands as libmw's CHANGE_INDEX
		// convention (used in peg-in change-on-mweb).
		if wallet.Kind == "mweb" || wallet.Kind == "universal" {
			for i := 0; i < count; i++ {
				if _, _, aErr := wallet.NewAddress(oyoltc.AddrMweb); aErr != nil {
					a.jsonError(w, "mweb address allocation: "+aErr.Error(), 500)
					return
				}
			}
		}
		// Bootstrap the canonical (p2wpkh) side via the rescan op — local
		// mirror under engine=mirror, node scantxoutset under legacy/both.
		// MWEB side gets bootstrapped separately if enabled.
		var canonicalBoot, mwebBoot json.RawMessage
		if wallet.Kind == "regular" || wallet.Kind == "universal" {
			r1, e1 := runWalletOp(a.rpc, wallet.RescanOp())
			if e1 != nil {
				a.jsonError(w, "p2wpkh bootstrap: "+e1.Error(), 502)
				return
			}
			canonicalBoot = json.RawMessage(r1)
		}
		if wallet.Kind == "mweb" || wallet.Kind == "universal" {
			r2, e2 := runWalletOp(a.rpc, wallet.BootstrapOp())
			if e2 != nil {
				a.jsonError(w, "mweb bootstrap: "+e2.Error(), 502)
				return
			}
			mwebBoot = json.RawMessage(r2)
		}
		a.jsonResponse(w, map[string]interface{}{
			"name":                name,
			"type":                "oyo_" + wallet.Kind,
			"kind":                wallet.Kind,
			"canonical_bootstrap": canonicalBoot,
			"mweb_bootstrap":      mwebBoot,
		})
	case "mweb":
		seed := r.URL.Query().Get("seed")
		if seed == "" {
			a.jsonError(w, "seed (hex64) required for mweb wallet", 400)
			return
		}
		birthHeight, bhErr := parseBirthHeight(r)
		if bhErr != nil {
			a.jsonError(w, bhErr.Error(), 400)
			return
		}
		wallet, err := a.wallets.CreateMweb(name, seed, birthHeight)
		if err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		// Pre-allocate `mwebPrefillCount` addresses up-front. Index 0
		// is libmw's CHANGE_INDEX (peg-in change destination); the rest
		// give the wallet a small ready-made batch of receive addresses
		// without forcing a runtime newaddress per use. Same default the
		// universal flow uses on its MWEB side. Bumps mwebAddressCount
		// for Unload→Load re-allocation.
		const mwebPrefillCount = 8
		var firstAddr string
		for i := 0; i < mwebPrefillCount; i++ {
			_, addr, aErr := wallet.NewAddress(oyoltc.AddrMweb)
			if aErr != nil {
				a.jsonError(w, "address allocation: "+aErr.Error(), 500)
				return
			}
			if i == 0 {
				firstAddr = addr
			}
		}
		boot, bErr := runWalletOp(a.rpc, wallet.BootstrapOp())
		if bErr != nil {
			a.jsonError(w, bErr.Error(), 502)
			return
		}
		a.jsonResponse(w, map[string]interface{}{
			"name":      name,
			"type":      "oyo_mweb",
			"address":   firstAddr,
			"bootstrap": json.RawMessage(boot),
		})
	case "seed":
		seed := r.URL.Query().Get("seed")
		if seed == "" {
			a.jsonError(w, "seed string required", 400)
			return
		}
		// 1. Create blank wallet (sethdseed will clear BLANK flag and init MWEB keychain)
		if _, err := a.rpc.CallRaw("createwallet", []interface{}{name, false, true}); err != nil {
			a.jsonError(w, err.Error(), 500)
			return
		}
		// 2. Derive WIF from seed string. The WIF network byte MUST match the
		// node's chain — a testnet-encoded key is rejected by sethdseed on
		// mainnet (RPC -5 Invalid private key). regtest shares testnet's byte.
		testnet := a.network != "main" && a.network != "mainnet"
		wif := SeedToWIF(seed, testnet)
		// 3. Set HD seed on the new wallet
		if _, err := a.rpc.CallWallet(name, "sethdseed", []interface{}{true, wif}); err != nil {
			a.jsonError(w, "sethdseed: "+err.Error(), 500)
			return
		}
		a.jsonResponse(w, map[string]interface{}{"name": name, "type": "seed"})
	default:
		a.jsonError(w, "invalid type: must be 'seed', 'regular', 'mweb', or 'universal' (hd/watch/blank no longer supported)", 400)
		return
	}
}

// POST /api/wallet/load?name=...
//
// For OYO wallets: re-opens the liboyoltc handle from the creation
// params kept in RAM since Create*. Idempotent on already-loaded
// wallets.
//
// For NODE wallets: forwards to litecoind's `loadwallet`.
func (a *API) handleWalletLoad(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if _, ok := a.wallets.Get(name); ok {
		state, err := a.wallets.Load(name)
		if err != nil {
			a.jsonError(w, err.Error(), 500)
			return
		}
		if state == "missing" {
			a.jsonError(w, "wallet not found", 404)
			return
		}
		// Re-bootstrap so the freshly-opened handle finds its UTXOs
		// again. Mirrors what Create did: P2WPKH side via the rescan op
		// (local mirror under engine=mirror, else node scantxoutset), MWEB
		// side via trial-decryption against the persistent mirror.
		// Skipped on already-loaded (idempotent Load is a no-op).
		if state == "loaded" {
			ext, _ := a.wallets.Get(name)
			if ext.Kind == "regular" || ext.Kind == "universal" {
				if _, bErr := runWalletOp(a.rpc, ext.RescanOp()); bErr != nil {
					log.Printf("load: p2wpkh rescan failed for %s: %v", name, bErr)
				}
			}
			if ext.Kind == "mweb" || ext.Kind == "universal" {
				if _, bErr := runWalletOp(a.rpc, ext.BootstrapOp()); bErr != nil {
					log.Printf("load: mweb bootstrap failed for %s: %v", name, bErr)
				}
			}
		}
		a.wallets.WakeTailer()
		a.jsonResponse(w, map[string]interface{}{"loaded": name, "oyo": true, "state": state})
		return
	}
	result, err := a.rpc.CallRaw("loadwallet", []interface{}{name})
	if err != nil {
		a.jsonError(w, err.Error(), 500)
		return
	}
	a.jsonResponse(w, map[string]interface{}{"loaded": name, "result": json.RawMessage(result)})
}

// POST /api/wallet/unload?name=...
//
// For OYO wallets: closes the liboyoltc handle but keeps the entry +
// creation params in the registry, so /api/wallet/load can reopen it
// without the user re-entering the seed. Reversible.
//
// For NODE wallets: forwards to litecoind's `unloadwallet`.
func (a *API) handleWalletUnload(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" && r.Method != "DELETE" {
		a.jsonError(w, "POST or DELETE required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if _, ok := a.wallets.Get(name); ok {
		state := a.wallets.Unload(name)
		if state == "missing" {
			a.jsonError(w, "wallet not found", 404)
			return
		}
		a.jsonResponse(w, map[string]interface{}{"unloaded": name, "oyo": true, "state": state})
		return
	}

	result, err := a.rpc.CallRaw("unloadwallet", []interface{}{name})
	if err != nil {
		a.jsonError(w, err.Error(), 500)
		return
	}
	a.jsonResponse(w, map[string]interface{}{"unloaded": name, "result": json.RawMessage(result)})
}

// POST /api/wallet/delete?name=...
//
// For OYO wallets: closes the handle (if loaded) and drops the registry
// entry + creation params permanently. Sole permanent removal — the
// only way to lose seed material from RAM other than process restart.
//
// For NODE wallets: forwards to oyo-deletewallet (file delete) and
// requires the wallet to already be unloaded by the node.
func (a *API) handleWalletDeleteFromDisk(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if _, ok := a.wallets.Get(name); ok {
		if !a.wallets.Delete(name) {
			a.jsonError(w, "wallet not found", 404)
			return
		}
		// Pending queue tasks for this wallet can no longer execute
		// (no wallet handle to build against). Mark them failed now
		// rather than at scheduled-fire time.
		if a.queue != nil {
			a.queue.FailWalletTasks(name, "wallet deleted")
		}
		a.jsonResponse(w, map[string]interface{}{"deleted": name, "oyo": true})
		return
	}
	if !a.hasOyoFeature("oyo-deletewallet") {
		a.jsonError(w, "node wallet delete is not available with vanilla litecoind; unload the wallet and remove it outside oyo-web", 501)
		return
	}
	result, err := a.rpc.CallRaw("oyo-deletewallet", []interface{}{name})
	if err != nil {
		a.jsonError(w, err.Error(), 500)
		return
	}
	if a.queue != nil {
		a.queue.FailWalletTasks(name, "wallet deleted")
	}
	a.jsonResponse(w, result)
}

// GET /api/wallet/info?name=...
//
// Returns a unified shape for both OYO and node wallets:
//
//	{
//	  name, kind, subkind, watch_only,
//	  confirmed_sat, available_sat,
//	  pending_in_sat, pending_out_sat, immature_sat,
//	  balances:  {mine: {trusted, untrusted_pending, immature, ...}},
//	  addresses: [{address, confirmed_sat, pending_in_sat, ...}, ...],
//	  mweb:      {balance_sat, addresses?: [...]}    // when applicable
//	}
//
// OYO wallets pass through StatusJSON. Node wallets get synthesized:
// getbalances → *_sat ints, listaddressgroupings + listreceivedbyaddress
// → addresses[], mempool overlay → pending_in_sat / pending_out_sat
// (split by direction, unlike getbalances.mine.untrusted_pending which
// is net). The synthesizer is intentionally pin-compatible with the
// OYO shape so tests / frontend can read either kind through the same
// path without forking.
func (a *API) handleWalletInfo(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if ext, ok := a.wallets.Get(name); ok {
		a.jsonResponse(w, json.RawMessage(ext.StatusJSON()))
		return
	}
	info, err := a.synthNodeWalletInfo(name)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	a.jsonResponse(w, info)
}

// synthNodeWalletInfo builds the OYO-shape /wallet/info response for
// a node-side HD wallet. Pulls balance buckets from getbalances,
// address book from listaddressgroupings + listreceivedbyaddress, and
// per-address pending overlays from the mempool watcher. Raw
// getwalletinfo fields (hdseedid, keypoolsize, scanning, ...) are
// merged at the top level so HD-introspection callers keep working.
func (a *API) synthNodeWalletInfo(name string) (json.RawMessage, error) {
	balRaw, err := a.rpc.CallWallet(name, "getbalances", nil)
	if err != nil {
		return nil, err
	}
	// getwalletinfo carries hd metadata + scanning state — fetch it
	// alongside getbalances so HD-introspection tests keep working.
	infoRaw, _ := a.rpc.CallWallet(name, "getwalletinfo", nil)
	var bal struct {
		Mine struct {
			Trusted              float64 `json:"trusted"`
			UntrustedPending     float64 `json:"untrusted_pending"`
			Immature             float64 `json:"immature"`
			MwebTrusted          float64 `json:"mweb_trusted,omitempty"`
			MwebUntrustedPending float64 `json:"mweb_untrusted_pending,omitempty"`
			MwebImmature         float64 `json:"mweb_immature,omitempty"`
		} `json:"mine"`
	}
	_ = json.Unmarshal(balRaw, &bal)

	const sat = 1e8
	// getbalances.mine.trusted is the unified total (canonical + MWEB).
	// OYO StatusJSON keeps the two sides apart: confirmed_sat is the
	// canonical-side trusted, mweb.balance_sat is the MWEB-side. Match
	// that convention so callers don't have to fork by wallet kind.
	mwebBalSat := int64(math.Round(bal.Mine.MwebTrusted * sat))
	confirmedSat := int64(math.Round(bal.Mine.Trusted*sat)) - mwebBalSat
	immatureSat := int64(math.Round(bal.Mine.Immature*sat) - math.Round(bal.Mine.MwebImmature*sat))

	// Address book — confirmed balance per address from groupings,
	// plus any "received but currently zero" address from received.
	type addrEntry struct {
		Address       string `json:"address"`
		ConfirmedSat  int64  `json:"confirmed_sat"`
		PendingInSat  int64  `json:"pending_in_sat,omitempty"`
		PendingOutSat int64  `json:"pending_out_sat,omitempty"`
	}
	addrMap := map[string]*addrEntry{}
	if groupings, gErr := a.rpc.CallWallet(name, "listaddressgroupings", nil); gErr == nil {
		var groups [][][]interface{}
		_ = json.Unmarshal(groupings, &groups)
		for _, group := range groups {
			for _, entry := range group {
				if len(entry) < 2 {
					continue
				}
				addr, _ := entry[0].(string)
				balLtc, _ := entry[1].(float64)
				if addr == "" {
					continue
				}
				addrMap[addr] = &addrEntry{Address: addr, ConfirmedSat: int64(math.Round(balLtc * sat))}
			}
		}
	}
	if received, rErr := a.rpc.CallWallet(name, "listreceivedbyaddress", []interface{}{0, true, true}); rErr == nil {
		var rows []struct {
			Address string `json:"address"`
		}
		_ = json.Unmarshal(received, &rows)
		for _, r := range rows {
			if addrMap[r.Address] == nil {
				addrMap[r.Address] = &addrEntry{Address: r.Address}
			}
		}
	}

	// Mempool overlay: per-address pending_in / pending_out, then
	// fold up to wallet-level totals so callers see the same fields
	// here that OYO wallets expose natively.
	var pendingInSat, pendingOutSat int64
	if a.mempool != nil {
		for addr, ap := range a.mempool.GetAddrPending() {
			ae := addrMap[addr]
			if ae == nil {
				continue // not our wallet's address
			}
			in := int64(math.Round(ap.Outputs * sat))
			out := int64(math.Round(ap.Inputs * sat))
			ae.PendingInSat = in
			ae.PendingOutSat = out
			pendingInSat += in
			pendingOutSat += out
		}
	}
	availableSat := confirmedSat - immatureSat
	addresses := make([]*addrEntry, 0, len(addrMap))
	for _, ae := range addrMap {
		addresses = append(addresses, ae)
	}

	// Start from the raw getwalletinfo fields so HD-introspection
	// (hdseedid / keypoolsize / private_keys_enabled / scanning ...)
	// stays accessible at top level. Then layer the OYO-shape fields
	// on top — they win on collisions like `name` because OYO's
	// definition of those is the unified contract.
	out := map[string]interface{}{}
	if len(infoRaw) > 0 {
		_ = json.Unmarshal(infoRaw, &out)
	}
	out["name"] = name
	out["kind"] = "node"
	out["subkind"] = "hd"
	out["watch_only"] = false
	out["confirmed_sat"] = confirmedSat
	out["available_sat"] = availableSat
	out["pending_in_sat"] = pendingInSat
	out["pending_out_sat"] = pendingOutSat
	out["immature_sat"] = immatureSat
	out["balances"] = json.RawMessage(balRaw)
	out["addresses"] = addresses
	out["mweb"] = map[string]interface{}{
		"balance_sat":     mwebBalSat,
		"pending_in_sat":  int64(math.Round(bal.Mine.MwebUntrustedPending * sat)),
		"pending_out_sat": int64(0), // node-side mempool doesn't separate MWEB direction
		"addresses":       []interface{}{},
	}
	return json.Marshal(out)
}

// GET /api/wallet/balances?name=...
func (a *API) handleWalletBalances(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if ext, ok := a.wallets.Get(name); ok {
		a.jsonResponse(w, json.RawMessage(ext.BalancesJSON()))
		return
	}
	balances, err := a.rpc.CallWallet(name, "getbalances", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}

	a.jsonResponse(w, json.RawMessage(balances))
}

// GET /api/wallet/addresses?name=...
func (a *API) handleWalletAddresses(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if ext, ok := a.wallets.Get(name); ok {
		a.jsonResponse(w, json.RawMessage(ext.AddressesJSON()))
		return
	}

	// Use getbalances for wallet totals (standard RPC)
	balancesRaw, err := a.rpc.CallWallet(name, "getbalances", nil)
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}

	// listaddressgroupings: confirmed balances per address
	groupings, _ := a.rpc.CallWallet(name, "listaddressgroupings", nil)

	// listreceivedbyaddress: all addresses (even empty)
	received, _ := a.rpc.CallWallet(name, "listreceivedbyaddress", []interface{}{0, true, true})

	// Build address list with mempool pending overlay
	type addrEntry struct {
		Address    string  `json:"address"`
		Confirmed  float64 `json:"confirmed"`
		PendingIn  float64 `json:"pending_in,omitempty"`
		PendingOut float64 `json:"pending_out,omitempty"`
	}

	addrMap := make(map[string]*addrEntry)

	// Confirmed balances from groupings
	if groupings != nil {
		var groups [][][]interface{}
		json.Unmarshal(groupings, &groups)
		for _, group := range groups {
			for _, entry := range group {
				if len(entry) >= 2 {
					addr, _ := entry[0].(string)
					bal, _ := entry[1].(float64)
					if addr != "" {
						addrMap[addr] = &addrEntry{Address: addr, Confirmed: bal}
					}
				}
			}
		}
	}

	// Add empty addresses from received
	if received != nil {
		var recvAddrs []struct {
			Address string  `json:"address"`
			Amount  float64 `json:"amount"`
		}
		json.Unmarshal(received, &recvAddrs)
		for _, r := range recvAddrs {
			if addrMap[r.Address] == nil {
				addrMap[r.Address] = &addrEntry{Address: r.Address}
			}
		}
	}

	// Overlay mempool pending only for addresses belonging to this wallet
	if a.mempool != nil {
		addrPending := a.mempool.GetAddrPending()
		for addr, ap := range addrPending {
			ae := addrMap[addr]
			if ae == nil {
				continue // not our wallet's address
			}
			ae.PendingIn = ap.Outputs // receiving
			ae.PendingOut = ap.Inputs // spending
		}
	}

	entries := make([]addrEntry, 0, len(addrMap))
	for _, ae := range addrMap {
		entries = append(entries, *ae)
	}

	// Calculate wallet-level net pending from address entries
	var netPending float64
	for _, ae := range entries {
		netPending += ae.PendingIn - ae.PendingOut
	}

	result := map[string]interface{}{
		"addresses":   entries,
		"balances":    json.RawMessage(balancesRaw),
		"net_pending": netPending,
	}
	a.jsonResponse(w, result)
}

// POST /api/wallet/newaddress?name=...&type=legacy|p2sh-segwit|bech32|mweb
func (a *API) handleWalletNewAddress(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if ext, ok := a.wallets.Get(name); ok {
		if !ext.Loaded() {
			a.jsonError(w, errUnloadedWallet.Error(), 409)
			return
		}
		// `?type=` selects the address kind. Canonical sides (regular /
		// universal) take p2wpkh (default) | legacy (P2PKH) | nested
		// (P2SH-P2WPKH) — all from the same oyo_v1 key (P2.1). MWEB takes
		// mweb. Universal with no type keeps the historical mweb default.
		want := r.URL.Query().Get("type")
		// canonicalKind maps a requested type to a liboyoltc canonical kind.
		canonicalKind := func(t string) (int32, string, bool) {
			switch t {
			case "", "p2wpkh", "bech32":
				return oyoltc.AddrP2wpkh, "p2wpkh", true
			case "legacy", "p2pkh":
				return oyoltc.AddrP2pkh, "p2pkh", true
			case "nested", "p2sh-segwit", "p2sh-p2wpkh":
				return oyoltc.AddrP2shP2wpkh, "p2sh-p2wpkh", true
			}
			return 0, "", false
		}
		emit := func(ak int32, kind string) {
			idx, addr, err := ext.NewAddress(ak)
			if err != nil {
				a.jsonError(w, err.Error(), 500)
				return
			}
			a.jsonResponse(w, map[string]interface{}{
				"address": addr, "index": idx, "kind": kind,
			})
		}
		switch ext.Kind {
		case "mweb":
			emit(oyoltc.AddrMweb, "mweb")
			return
		case "regular":
			ak, kind, ok := canonicalKind(want)
			if !ok {
				a.jsonError(w, "unsupported address type for regular wallet: "+want, 400)
				return
			}
			emit(ak, kind)
			return
		case "universal":
			if want == "" || want == "mweb" {
				emit(oyoltc.AddrMweb, "mweb") // historical default
				return
			}
			ak, kind, ok := canonicalKind(want)
			if !ok {
				a.jsonError(w, "unsupported address type for universal wallet: "+want, 400)
				return
			}
			emit(ak, kind)
			return
		}
		// Watch-only: addresses are user-supplied via /api/wallet/import-address.
		a.jsonError(w, "newaddress is not supported for watch-only wallets — use import-address", 501)
		return
	}

	addrType := r.URL.Query().Get("type")
	label := r.URL.Query().Get("label")

	var params []interface{}
	if addrType != "" {
		params = []interface{}{label, addrType}
	} else if label != "" {
		params = []interface{}{label}
	}

	addr, err := a.rpc.CallWallet(name, "getnewaddress", params)
	if err != nil {
		a.jsonError(w, err.Error(), 500)
		return
	}
	a.jsonResponse(w, map[string]json.RawMessage{"address": addr})
}

// GET /api/wallet/history?name=...&count=20
func (a *API) handleWalletHistory(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}

	count := 20
	if c := r.URL.Query().Get("count"); c != "" {
		n, err := strconv.Atoi(c)
		if err == nil && n > 0 && n <= 100 {
			count = n
		}
	}

	if ext, ok := a.wallets.Get(name); ok {
		// OYO wallets — derive history from the visible UTXO set
		// (receive/pending_in/pending_out events). Confirmed sends
		// are not surfaced yet (status JSON drops fully-spent UTXOs);
		// proper full-history needs a C-side event log.
		a.jsonResponse(w, ext.HistoryJSON(count))
		return
	}

	txs, err := a.rpc.CallWallet(name, "listtransactions", []interface{}{"*", count})
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	a.jsonResponse(w, txs)
}

// extractOpError pulls the structured error message out of liboyoltc's
// op result body (shape: {"status":"error","error":"..."}). Falls back
// to the rc-string from RunOp when the result is empty or malformed —
// preserves callers that depend on the prior error text.
func extractOpError(res []byte, fallback error) string {
	if len(res) > 0 {
		var parsed struct {
			Status string `json:"status"`
			Error  string `json:"error"`
		}
		if json.Unmarshal(res, &parsed) == nil && parsed.Error != "" {
			return parsed.Error
		}
	}
	if fallback != nil {
		return fallback.Error()
	}
	return "operation failed"
}

// sendBody describes the optional JSON body the multi-recipient and
// manual-input flows use. Both /api/wallet/send and /api/wallet/estimate-send
// accept it; URL params (to/amount/send_all) remain the legacy single-
// recipient path. Empty body → fall back to URL params, preserving the
// queue + tests + bookmarklet programmatic interface unchanged.
//
// Inputs entries take {txid, vout} for canonical (P2WPKH) paths and
// {commitment} for the MWEB path. The lib enforces shape per dispatch
// kind; Go just passes whatever the caller sent.
type sendBody struct {
	Outputs []struct {
		Address   string `json:"address"`
		AmountSat int64  `json:"amount_sat"`
		// Max (P2.4): this output absorbs the remainder; amount_sat ignored.
		Max bool `json:"max,omitempty"`
	} `json:"outputs"`
	Inputs []struct {
		TxID       string `json:"txid,omitempty"`
		Vout       uint32 `json:"vout,omitempty"`
		Commitment string `json:"commitment,omitempty"`
	} `json:"inputs"`
	// ChangeAddress (P2.3): canonical change destination override.
	ChangeAddress string `json:"change_address,omitempty"`
}

func decodeSendBody(r *http.Request) (sendBody, error) {
	var body sendBody
	if r.Body == nil {
		return body, nil
	}
	// Read at most a small bound to guard against malformed clients;
	// real outputs[] payloads are tiny (<1 KB even with 50 recipients).
	const maxSendBodyBytes = 64 * 1024
	raw, err := io.ReadAll(io.LimitReader(r.Body, maxSendBodyBytes))
	if err != nil || len(raw) == 0 {
		return body, err
	}
	// Tolerate empty body / non-JSON bodies (legacy clients set
	// Content-Type: application/x-www-form-urlencoded but send no body).
	if err := json.Unmarshal(raw, &body); err != nil {
		return sendBody{}, nil
	}
	return body, nil
}

// POST /api/wallet/send?name=...&to=...&amount=...
//
// Two flow shapes:
//
//  1. Confirm-after-estimate: caller passes ?confirm_token=<txid> from a
//     prior /api/wallet/estimate-send. The cached signed tx_hex is
//     broadcast verbatim — same tx the user saw and approved in the
//     pre-broadcast modal. No re-build, no fee re-estimation.
//
//  2. Direct: caller passes ?to=&amount= (or send_all=true). Server
//     builds, signs, broadcasts in one shot — legacy/programmatic path.
//     Used by the queue and tests; the UI always goes through (1).
//
// engineReadyOrErr drives the engine to the node tip and returns true only when
// it is fully caught up. OYO tx formation (estimate) and broadcast (send) are
// blocked while the local mirror is still walking toward the node — coin
// selection on an incomplete UTXO view could pick already-spent inputs or miss
// new ones. SyncOnce runs first so a transient one-block tailer lag doesn't
// false-trip the gate (it just applies the block and reports "synced"); a
// cold-start genesis walk (chunked → "partial")
// keeps the gate closed with a 503 the UI can surface. The confirm_token
// broadcast path is intentionally NOT gated — it ships a tx that was already
// formed (and gated) at estimate time.
func (a *API) engineReadyOrErr(w http.ResponseWriter) bool {
	if a.wallets == nil {
		return true
	}
	res, err := a.wallets.SyncOnce()
	if err != nil {
		a.jsonError(w, "engine not ready: sync failed: "+err.Error(), 503)
		return false
	}
	var st struct {
		Status    string `json:"status"`
		TipHeight int64  `json:"tip_height"`
	}
	_ = json.Unmarshal(res, &st)
	if st.Status != "synced" {
		a.jsonError(w, fmt.Sprintf("engine is catching up to the node (tip %d) — wallet sends are paused until it is in sync", st.TipHeight), 503)
		return false
	}
	return true
}

func (a *API) handleWalletSend(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}

	// Token path: short-circuit before any param validation. The token IS
	// the txid; cache holds the matching signed hex.
	if token := r.URL.Query().Get("confirm_token"); token != "" {
		if a.confirms == nil {
			a.jsonError(w, "confirm cache disabled", 500)
			return
		}
		entry, ok := a.confirms.take(token)
		if !ok {
			a.jsonError(w, "confirm token expired or unknown — re-estimate the send", 410)
			return
		}
		if entry.wallet != name {
			a.jsonError(w, "confirm token does not match this wallet", 400)
			return
		}
		txidRaw, err := a.rpc.CallRaw("sendrawtransaction", []interface{}{entry.txHex})
		if err != nil {
			log.Printf("confirm-broadcast failed for %s token=%s: %v", name, token, err)
			a.jsonError(w, err.Error(), 502)
			return
		}
		var txid string
		_ = json.Unmarshal(txidRaw, &txid)
		if a.mempool != nil {
			a.mempool.Refresh()
		}
		if a.wallets != nil {
			a.wallets.WakeTailer()
		}
		log.Printf("confirm-broadcast %s from wallet %s", txid, name)
		a.jsonResponse(w, map[string]interface{}{"status": "sent", "txid": txid})
		return
	}

	// Optional JSON body with multi-recipient outputs[] / future inputs[].
	// Body has priority over URL params (to/amount) when present.
	body, _ := decodeSendBody(r)

	to := r.URL.Query().Get("to")
	amountStr := r.URL.Query().Get("amount")
	sendAllParam := r.URL.Query().Get("send_all") == "true"
	// Same guard as estimate-send: send_all drains to a single
	// recipient, outputs[] declares N. Mutually exclusive.
	if sendAllParam && len(body.Outputs) > 0 {
		a.jsonError(w, "send_all is incompatible with outputs[] body", 400)
		return
	}
	if len(body.Outputs) == 0 {
		if to == "" {
			a.jsonError(w, "to required", 400)
			return
		}
		if amountStr == "" && !sendAllParam {
			a.jsonError(w, "amount required (or send_all=true)", 400)
			return
		}
	}
	var amount float64
	if amountStr != "" {
		var err error
		amount, err = strconv.ParseFloat(amountStr, 64)
		if err != nil || amount <= 0 {
			a.jsonError(w, "invalid amount", 400)
			return
		}
	}

	if ext, ok := a.wallets.Get(name); ok {
		if !a.engineReadyOrErr(w) {
			return
		}
		// Auto-fee always-on. Optional `fee_rate_sat_per_vb` URL param
		// overrides the estimatesmartfee lookup. `send_all` makes the
		// wallet drain all confirmed UTXOs (recipient = total - fee).
		// `dry_run` builds + signs but skips broadcast and returns the
		// would-be result; powers the /api/wallet/estimate-send endpoint.
		feeRate := int64(0)
		if raw := r.URL.Query().Get("fee_rate_sat_per_vb"); raw != "" {
			n, fErr := strconv.ParseInt(raw, 10, 64)
			if fErr != nil || n < 1 {
				a.jsonError(w, "invalid fee_rate_sat_per_vb", 400)
				return
			}
			feeRate = n
		}
		sendAll := r.URL.Query().Get("send_all") == "true"
		dryRun := r.URL.Query().Get("dry_run") == "true"
		amountSat := int64(0)
		if !sendAll {
			amountSat = int64(math.Round(amount * 1e8))
		}

		buildCfg := func() []byte {
			cfg := map[string]interface{}{}
			if len(body.Outputs) > 0 {
				outs := make([]map[string]interface{}, len(body.Outputs))
				for i, o := range body.Outputs {
					outs[i] = map[string]interface{}{
						"address":    o.Address,
						"amount_sat": o.AmountSat,
					}
					if o.Max {
						outs[i]["max"] = true
					}
				}
				cfg["outputs"] = outs
			} else {
				cfg["to"] = to
				if sendAll {
					cfg["send_all"] = true
				} else {
					cfg["amount_sat"] = amountSat
				}
			}
			if len(body.Inputs) > 0 {
				ins := make([]map[string]interface{}, len(body.Inputs))
				for i, in := range body.Inputs {
					m := map[string]interface{}{}
					if in.TxID != "" {
						m["txid"] = in.TxID
						m["vout"] = in.Vout
					}
					if in.Commitment != "" {
						m["commitment"] = in.Commitment
					}
					ins[i] = m
				}
				cfg["inputs"] = ins
			}
			if body.ChangeAddress != "" {
				cfg["change_address"] = body.ChangeAddress
			}
			if feeRate > 0 {
				cfg["fee_rate_sat_per_vb"] = feeRate
			}
			if dryRun {
				cfg["dry_run"] = true
			}
			b, _ := json.Marshal(cfg)
			return b
		}

		// Single dispatch entry — liboyoltc's oyo_wallet_send picks
		// regular/mweb/pegin internally based on wallet kind + dest class.
		op := ext.SendOp(buildCfg())
		res, opErr := runWalletOp(a.rpc, op)
		// Log line covers both single-recipient (to/amountSat) and
		// multi-recipient (body.Outputs) paths — pick whichever is set.
		logTo := to
		logAmt := amountSat
		if len(body.Outputs) > 0 {
			logTo = body.Outputs[0].Address
			if len(body.Outputs) > 1 {
				logTo += fmt.Sprintf(" (+%d more)", len(body.Outputs)-1)
			}
			logAmt = 0
			for _, o := range body.Outputs {
				logAmt += o.AmountSat
			}
		}
		if opErr != nil {
			log.Printf("send failed for %s amount=%d to=%s: %v (result=%s)",
				name, logAmt, logTo, opErr, string(res))
			status := 502
			if errors.Is(opErr, errUnloadedWallet) {
				status = 409
			}
			// Surface liboyoltc's structured error message to the caller
			// when present. Falls back to the raw rc-string from RunOp.
			a.jsonError(w, extractOpError(res, opErr), status)
			return
		}
		a.wallets.WakeTailer()
		log.Printf("send %d sat to %s from OYO wallet %s (kind=%s)", logAmt, logTo, name, ext.Kind)
		a.jsonResponse(w, json.RawMessage(res))
		return
	}

	txid, err := a.rpc.CallWallet(name, "sendtoaddress", []interface{}{to, amount})
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}

	log.Printf("Sent %f LTC to %s from wallet %s", amount, to, name)
	// Refresh mempool watcher immediately so pending shows up
	if a.mempool != nil {
		a.mempool.Refresh()
	}
	if a.wallets != nil {
		a.wallets.WakeTailer()
	}
	a.jsonResponse(w, map[string]json.RawMessage{"txid": txid})
}

// GET /api/wallet/estimate-send?name=...&to=...&[amount=...|send_all=true][&fee_rate_sat_per_vb=N]
//
// Runs the same op-pump as /api/wallet/send but with dry_run=true, so the
// tx is built+signed but never broadcast. The returned JSON contains the
// recipient amount, fee, fee rate, and change/inputs breakdown — used by
// the frontend's Max button to populate the Amount field with the exact
// number the recipient will receive after fees.
func (a *API) handleWalletEstimateSend(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	body, _ := decodeSendBody(r)
	to := r.URL.Query().Get("to")
	if len(body.Outputs) == 0 && to == "" {
		a.jsonError(w, "to required", 400)
		return
	}
	sendAll := r.URL.Query().Get("send_all") == "true"
	// send_all + outputs[] is meaningless: send_all drains the wallet
	// to a single recipient, but outputs[] declares N recipients with
	// fixed amounts. Reject the combination here so callers see the
	// same contract the lib enforces (outputs[] incompatible with
	// send_all) instead of one side silently winning.
	if sendAll && len(body.Outputs) > 0 {
		a.jsonError(w, "send_all is incompatible with outputs[] body", 400)
		return
	}
	amountStr := r.URL.Query().Get("amount")
	var amountSat int64
	if len(body.Outputs) == 0 && !sendAll {
		if amountStr == "" {
			a.jsonError(w, "amount required (or send_all=true)", 400)
			return
		}
		f, err := strconv.ParseFloat(amountStr, 64)
		if err != nil || f <= 0 {
			a.jsonError(w, "invalid amount", 400)
			return
		}
		amountSat = int64(math.Round(f * 1e8))
	}
	feeRate := int64(0)
	if raw := r.URL.Query().Get("fee_rate_sat_per_vb"); raw != "" {
		n, fErr := strconv.ParseInt(raw, 10, 64)
		if fErr != nil || n < 1 {
			a.jsonError(w, "invalid fee_rate_sat_per_vb", 400)
			return
		}
		feeRate = n
	}

	// External (OYO) wallets: liboyoltc op-pump in dry_run mode.
	// Node wallets: oyo-send RPC with dry_run=true. Both paths build
	// and sign the tx, return tx_hex + summary fields, and cache the
	// signed bytes by token so /api/wallet/send?confirm_token= can
	// broadcast the exact same bytes.
	var res json.RawMessage
	if ext, ok := a.wallets.Get(name); ok {
		if !a.engineReadyOrErr(w) {
			return
		}
		buildCfg := func() []byte {
			cfg := map[string]interface{}{"dry_run": true}
			if len(body.Outputs) > 0 {
				outs := make([]map[string]interface{}, len(body.Outputs))
				for i, o := range body.Outputs {
					outs[i] = map[string]interface{}{
						"address":    o.Address,
						"amount_sat": o.AmountSat,
					}
					if o.Max {
						outs[i]["max"] = true
					}
				}
				cfg["outputs"] = outs
			} else {
				cfg["to"] = to
				if sendAll {
					cfg["send_all"] = true
				} else {
					cfg["amount_sat"] = amountSat
				}
			}
			if len(body.Inputs) > 0 {
				ins := make([]map[string]interface{}, len(body.Inputs))
				for i, in := range body.Inputs {
					m := map[string]interface{}{}
					if in.TxID != "" {
						m["txid"] = in.TxID
						m["vout"] = in.Vout
					}
					if in.Commitment != "" {
						m["commitment"] = in.Commitment
					}
					ins[i] = m
				}
				cfg["inputs"] = ins
			}
			if body.ChangeAddress != "" {
				cfg["change_address"] = body.ChangeAddress
			}
			if feeRate > 0 {
				cfg["fee_rate_sat_per_vb"] = feeRate
			}
			b, _ := json.Marshal(cfg)
			return b
		}
		op := ext.SendOp(buildCfg())
		var opErr error
		res, opErr = runWalletOp(a.rpc, op)
		if opErr != nil {
			status := 502
			if errors.Is(opErr, errUnloadedWallet) {
				status = 409
			}
			a.jsonError(w, extractOpError(res, opErr), status)
			return
		}
	} else {
		// Node wallet: dispatch through oyo-send dry_run. Multi-recipient
		// is not supported for node wallets — the body.Outputs path is
		// rejected with 400 (callers should use a single `to`+`amount`).
		if !a.hasOyoFeature("oyo-send") {
			a.jsonError(w, "node wallet estimate-send requires oyo-send; simple node sends can still use /api/wallet/send directly", 501)
			return
		}
		if len(body.Outputs) > 1 {
			a.jsonError(w, "multi-recipient is not supported for node wallets", 400)
			return
		}
		dest := to
		amt := float64(amountSat) / 1e8
		if len(body.Outputs) == 1 {
			dest = body.Outputs[0].Address
			amt = float64(body.Outputs[0].AmountSat) / 1e8
		}
		options := map[string]interface{}{"dry_run": true}
		if sendAll {
			options["send_all"] = true
		}
		if feeRate > 0 {
			options["fee_rate"] = float64(feeRate) / 1e8
		}
		raw, err := a.rpc.CallWallet(name, "oyo-send", []interface{}{dest, amt, options})
		if err != nil {
			a.jsonError(w, "oyo-send dry_run: "+err.Error(), 502)
			return
		}
		// Map oyo-send fields to the same shape the OYO wallet path emits
		// so the frontend modal renders identically — full inputs / outputs
		// breakdown included so the user can see address-level value flow.
		var got struct {
			Txid           string          `json:"txid"`
			TxHex          string          `json:"tx_hex"`
			FeeSat         int64           `json:"fee_sat"`
			Vsize          int64           `json:"vsize"`
			FeeRateSatPerV int64           `json:"fee_rate_sat_per_vb"`
			AmountSat      int64           `json:"amount_sat"`
			Path           string          `json:"path"`
			Inputs         json.RawMessage `json:"inputs"`
			Outputs        json.RawMessage `json:"outputs"`
		}
		_ = json.Unmarshal(raw, &got)
		out := map[string]interface{}{
			"txid":                got.Txid,
			"tx_hex":              got.TxHex,
			"fee_sat":             got.FeeSat,
			"vsize":               got.Vsize,
			"fee_rate_sat_per_vb": got.FeeRateSatPerV,
			"amount_sat":          got.AmountSat,
			"path":                got.Path,
			"inputs":              got.Inputs,
			"outputs":             got.Outputs,
		}
		res, _ = json.Marshal(out)
	}

	// Run testmempoolaccept against the signed tx hex so the modal can
	// gate the Confirm button on the node's actual would-accept verdict.
	var meta struct {
		TxID  string `json:"txid"`
		TxHex string `json:"tx_hex"`
	}
	_ = json.Unmarshal(res, &meta)
	enriched := map[string]interface{}{}
	_ = json.Unmarshal(res, &enriched)
	wouldAccept := true
	rejectReason := ""
	if meta.TxHex != "" {
		acceptRaw, err := a.rpc.CallRaw("testmempoolaccept",
			[]interface{}{[]string{meta.TxHex}})
		if err == nil {
			var accept []struct {
				Allowed      bool   `json:"allowed"`
				RejectReason string `json:"reject-reason"`
			}
			if json.Unmarshal(acceptRaw, &accept) == nil && len(accept) > 0 {
				wouldAccept = accept[0].Allowed
				rejectReason = accept[0].RejectReason
			}
		}
	}
	enriched["would_accept"] = wouldAccept
	if rejectReason != "" {
		enriched["reject_reason"] = rejectReason
	}

	if a.confirms != nil && wouldAccept && meta.TxID != "" && meta.TxHex != "" {
		a.confirms.put(meta.TxID, meta.TxHex, name)
		enriched["confirm_token"] = meta.TxID
	}

	final, _ := json.Marshal(enriched)
	a.jsonResponse(w, json.RawMessage(final))
}

// GET /api/wallet/utxos?name=... - list all spendable UTXOs (regular + MWEB)
func (a *API) handleWalletUTXOs(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if ext, ok := a.wallets.Get(name); ok {
		a.jsonResponse(w, json.RawMessage(ext.UTXOsJSON()))
		return
	}
	utxosRaw, err := a.rpc.CallWallet(name, "listunspent", []interface{}{1}) // confirmed only
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}
	var allUtxos []map[string]interface{}
	json.Unmarshal(utxosRaw, &allUtxos)
	a.jsonResponse(w, allUtxos)
}

// GET /api/wallet/secrets?name=...&confirm=yes
//
// DANGEROUS: returns the wallet's seed string + derived private keys (WIF)
// and MWEB scan/spend secrets — full spend authority. Gated behind an
// explicit ?confirm=yes so a stray UI request can't leak it; whatever auth
// the server runs (webuser/password) already applies to every /api route.
// OYO wallets only (node wallets export via the node's own dumpprivkey /
// backup; watch-only wallets have no secrets).
func (a *API) handleWalletSecrets(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if r.URL.Query().Get("confirm") != "yes" {
		a.jsonError(w, "refusing to reveal secrets without ?confirm=yes — this exposes full spend authority", 400)
		return
	}
	ext, ok := a.wallets.Get(name)
	if !ok {
		a.jsonError(w, "secrets export is only available for OYO wallets", 501)
		return
	}
	secrets, err := ext.SecretsJSON()
	if err != nil {
		status := 500
		if errors.Is(err, errUnloadedWallet) {
			status = 409
		}
		a.jsonError(w, err.Error(), status)
		return
	}
	log.Printf("DANGER: secrets revealed for OYO wallet %s", name)
	a.jsonResponse(w, json.RawMessage(secrets))
}

// GET /api/wallet/export?name=... - backup wallet as binary .dat (oyo-backupwallet)
func (a *API) handleWalletExport(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	if _, ok := a.wallets.Get(name); ok {
		a.jsonError(w, "export is not supported for OYO wallets — no .dat file exists", 501)
		return
	}
	if !a.hasOyoFeature("oyo-backupwallet") || !a.hasOyoFeature("oyo-restorewallet") || !a.hasOyoFeature("oyo-deletewallet") {
		a.jsonError(w, "node wallet export/verification requires OYO node wallet RPCs", 501)
		return
	}

	// Step 1: oyo-backupwallet reads unloaded wallet from disk
	backupResult, err := a.rpc.CallRaw("oyo-backupwallet", []interface{}{name})
	if err != nil {
		a.jsonError(w, err.Error(), 500)
		return
	}
	var backup struct {
		Data string `json:"data"`
		Size int64  `json:"size"`
	}
	json.Unmarshal(backupResult, &backup)

	// Step 2: Verify by restoring to temp, loading, comparing with original
	tempName := "_verify_" + name + "_" + fmt.Sprintf("%d", time.Now().UnixMilli())

	// Restore to temp
	_, err = a.rpc.CallRaw("oyo-restorewallet", []interface{}{tempName, backup.Data})
	if err != nil {
		a.jsonError(w, "backup verification failed (restore): "+err.Error(), 500)
		return
	}

	// Load both original and temp
	a.rpc.CallRaw("loadwallet", []interface{}{name})
	a.rpc.CallRaw("loadwallet", []interface{}{tempName})

	// Compare
	origInfo, err1 := a.rpc.CallWallet(name, "getwalletinfo", nil)
	tempInfo, err2 := a.rpc.CallWallet(tempName, "getwalletinfo", nil)

	// Unload both
	a.rpc.CallRaw("unloadwallet", []interface{}{name})
	a.rpc.CallRaw("unloadwallet", []interface{}{tempName})
	// Delete temp
	a.rpc.CallRaw("oyo-deletewallet", []interface{}{tempName})

	if err1 != nil || err2 != nil {
		a.jsonError(w, "backup verification failed (load)", 500)
		return
	}

	// Parse and compare key fields
	var orig, temp map[string]interface{}
	json.Unmarshal(origInfo, &orig)
	json.Unmarshal(tempInfo, &temp)

	checks := []string{"hdseedid", "txcount", "keypoolsize"}
	for _, key := range checks {
		if fmt.Sprintf("%v", orig[key]) != fmt.Sprintf("%v", temp[key]) {
			a.jsonError(w, fmt.Sprintf("backup verification failed: %s mismatch (orig=%v, copy=%v)", key, orig[key], temp[key]), 500)
			return
		}
	}

	log.Printf("Wallet export verified: %s (%d bytes)", name, backup.Size)

	// Step 3: Send binary .dat to client
	decoded, err := base64.StdEncoding.DecodeString(backup.Data)
	if err != nil {
		a.jsonError(w, "base64 decode error", 500)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%s.dat", name))
	w.Write(decoded)
}

// POST /api/wallet/import?name=... - restore wallet from binary .dat file
func (a *API) handleWalletImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if !a.hasOyoFeature("oyo-restorewallet") || !a.hasOyoFeature("oyo-deletewallet") {
		a.jsonError(w, "node wallet import requires OYO node wallet RPCs", 501)
		return
	}
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}

	defer r.Body.Close()
	body, err := io.ReadAll(io.LimitReader(r.Body, 50*1024*1024)) // 50MB limit for binary wallet
	if err != nil {
		a.jsonError(w, "failed to read body", 400)
		return
	}

	if len(body) == 0 {
		a.jsonError(w, "wallet file required", 400)
		return
	}

	// Step 1: Encode to base64 and restore via RPC
	b64 := base64.StdEncoding.EncodeToString(body)
	_, err = a.rpc.CallRaw("oyo-restorewallet", []interface{}{name, b64})
	if err != nil {
		a.jsonError(w, "restore failed: "+err.Error(), 500)
		return
	}

	// Step 2: Load and verify
	_, err = a.rpc.CallRaw("loadwallet", []interface{}{name})
	if err != nil {
		// Cleanup on failure
		a.rpc.CallRaw("oyo-deletewallet", []interface{}{name})
		a.jsonError(w, "wallet load failed (corrupt file?): "+err.Error(), 500)
		return
	}

	infoRaw, err := a.rpc.CallWallet(name, "getwalletinfo", nil)
	if err != nil {
		a.rpc.CallRaw("unloadwallet", []interface{}{name})
		a.rpc.CallRaw("oyo-deletewallet", []interface{}{name})
		a.jsonError(w, "wallet verification failed: "+err.Error(), 500)
		return
	}

	// Unload (user gets unloaded wallet, can Load from UI)
	a.rpc.CallRaw("unloadwallet", []interface{}{name})

	log.Printf("Wallet imported: %s (%d bytes)", name, len(body))
	a.jsonResponse(w, map[string]interface{}{
		"status": "imported",
		"name":   name,
		"size":   len(body),
		"info":   json.RawMessage(infoRaw),
	})
}

// GET /api/wallet/rescan?name=... - get rescan status
// POST /api/wallet/rescan?name=...&action=start - start rescan
// POST /api/wallet/rescan?name=...&action=abort - abort rescan
// POST /api/wallet/shadow?source=<node-wallet>&name=<shadow-name>
//
// Builds a shadow OYO wallet that mirrors all addresses currently
// known to the node-side wallet <source>. Combines getaddressesbylabel(""),
// getaddressesbylabel(label) for each labelled bucket, and
// listreceivedbyaddress(0,true,true) so we get both used and unused addrs.
// MWEB-style and other unsupported address kinds are skipped (counted in
// "skipped"). After registration runs an initial scantxoutset rescan so
// the shadow's confirmed UTXO set is in sync immediately.
func (a *API) handleWalletShadow(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if a.wallets == nil {
		a.jsonError(w, "OYO wallet registry not initialised", 503)
		return
	}
	source := r.URL.Query().Get("source")
	name := r.URL.Query().Get("name")
	if source == "" || name == "" {
		a.jsonError(w, "source and name required", 400)
		return
	}
	if name == source {
		a.jsonError(w, "shadow name must differ from source", 400)
		return
	}

	addrs, err := a.collectWalletAddresses(source)
	if err != nil {
		a.jsonError(w, fmt.Sprintf("collecting addresses from %s: %v", source, err), 502)
		return
	}
	if len(addrs) == 0 {
		a.jsonError(w, fmt.Sprintf("source %s has no known addresses to mirror", source), 400)
		return
	}

	ext, err := a.wallets.CreateWatch(name)
	if err != nil {
		a.jsonError(w, err.Error(), 400)
		return
	}

	added := 0
	skipped := []map[string]string{}
	for _, addr := range addrs {
		if _, err := ext.AddWatchAddress(addr); err != nil {
			skipped = append(skipped, map[string]string{"address": addr, "error": err.Error()})
			continue
		}
		added++
	}

	rescan, rerr := runWalletOp(a.rpc, ext.RescanOp())
	resp := map[string]interface{}{
		"name":            name,
		"source":          source,
		"addresses_total": len(addrs),
		"added":           added,
		"skipped":         skipped,
	}
	if rerr != nil {
		resp["rescan_error"] = rerr.Error()
		resp["rescan"] = json.RawMessage(rescan)
	} else {
		resp["rescan"] = json.RawMessage(rescan)
	}
	a.jsonResponse(w, resp)
}

// POST /api/wallet/shadow/refresh?source=<node-wallet>&name=<shadow>
//
// Re-enumerates addresses from <source> and adds anything new to the
// existing watch-only shadow. Necessary after node-wallet operations
// that mint new addresses (e.g. send-with-change), since a watch-only
// mirror cannot derive them on its own.
func (a *API) handleWalletShadowRefresh(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if a.wallets == nil {
		a.jsonError(w, "OYO wallet registry not initialised", 503)
		return
	}
	source := r.URL.Query().Get("source")
	name := r.URL.Query().Get("name")
	if source == "" || name == "" {
		a.jsonError(w, "source and name required", 400)
		return
	}
	ext, ok := a.wallets.Get(name)
	if !ok {
		a.jsonError(w, fmt.Sprintf("shadow %s not found", name), 404)
		return
	}
	addrs, err := a.collectWalletAddresses(source)
	if err != nil {
		a.jsonError(w, fmt.Sprintf("collecting addresses: %v", err), 502)
		return
	}
	added := 0
	for _, addr := range addrs {
		if _, err := ext.AddWatchAddress(addr); err == nil {
			added++
		}
		// existing-binding errors are silently ignored — that's the point of refresh.
	}
	res, rerr := runWalletOp(a.rpc, ext.RescanOp())
	resp := map[string]interface{}{
		"name":            name,
		"source":          source,
		"addresses_total": len(addrs),
		"newly_added":     added,
	}
	if rerr != nil {
		resp["rescan_error"] = rerr.Error()
	}
	resp["rescan"] = json.RawMessage(res)
	a.jsonResponse(w, resp)
}

// collectWalletAddresses returns every address tracked by the named
// node-wallet. Uses listreceivedbyaddress(0,true,true) which includes
// empty (no-receive) entries when include_empty=true. Then merges
// getaddressesbylabel("") for any addrs without received history that
// the node nonetheless considers wallet-owned.
func (a *API) collectWalletAddresses(source string) ([]string, error) {
	out := map[string]bool{}

	// listreceivedbyaddress: minconf=0, include_empty=true, include_watchonly=true
	recvRaw, err := a.rpc.CallWallet(source, "listreceivedbyaddress", []interface{}{0, true, true})
	if err == nil {
		var recv []map[string]interface{}
		_ = json.Unmarshal(recvRaw, &recv)
		for _, r := range recv {
			if s, ok := r["address"].(string); ok && s != "" {
				out[s] = true
			}
		}
	}

	// getaddressesbylabel("") fills in the rest (newly-generated addrs
	// the node has issued via getnewaddress that haven't received yet).
	labelRaw, err := a.rpc.CallWallet(source, "getaddressesbylabel", []interface{}{""})
	if err == nil {
		var labelMap map[string]interface{}
		_ = json.Unmarshal(labelRaw, &labelMap)
		for k := range labelMap {
			if k != "" {
				out[k] = true
			}
		}
	}

	// listunspent(0) catches change addresses that node-wallet just minted
	// for an in-flight send: those don't show up in listreceivedbyaddress
	// or getaddressesbylabel until the tx is confirmed, but they do appear
	// in listunspent with minconf=0.
	unspRaw, err := a.rpc.CallWallet(source, "listunspent", []interface{}{0, 9999999})
	if err == nil {
		var unsp []map[string]interface{}
		_ = json.Unmarshal(unspRaw, &unsp)
		for _, u := range unsp {
			if s, ok := u["address"].(string); ok && s != "" {
				out[s] = true
			}
		}
	}

	addrs := make([]string, 0, len(out))
	for k := range out {
		addrs = append(addrs, k)
	}
	return addrs, nil
}

// POST /api/chain/mempool-sync — drive a single mempool diff pass.
func (a *API) handleChainMempoolSync(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if a.wallets == nil {
		a.jsonError(w, "OYO wallet registry not initialised", 503)
		return
	}
	// One mempool sync pass drives both the canonical (P2WPKH) diff and the
	// MWEB diff — the latter is folded into the same getrawtransaction walk,
	// so there is no node-side oyo-mweb-mempool RPC anymore. The MWEB counts
	// ride under the "mweb" key of the result; the legacy top-level fields
	// ({status, mempool_added_count, ...}) are preserved for existing callers.
	res, err := a.wallets.MempoolSyncOnce()
	if err != nil {
		a.jsonError(w, fmt.Sprintf("%v: %s", err, string(res)), 502)
		return
	}
	var combined map[string]interface{}
	if uerr := json.Unmarshal(res, &combined); uerr != nil || combined == nil {
		combined = map[string]interface{}{"legacy_raw": json.RawMessage(res)}
	}
	a.jsonResponse(w, combined)
}

// POST /api/chain/sync — drive the liboyoltc chain sync op once (no rescan).
// Used by polling clients and the e2e suite to bring the in-memory UTXO set
// up to the node's tip without going through scantxoutset.
func (a *API) handleChainSync(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if a.wallets == nil {
		a.jsonError(w, "OYO wallet registry not initialised", 503)
		return
	}
	res, err := a.wallets.SyncOnce()
	if err != nil {
		a.jsonError(w, fmt.Sprintf("%v: %s", err, string(res)), 502)
		return
	}
	a.jsonResponse(w, json.RawMessage(res))
}

// handleWalletRescan — only OYO wallets are supported.
//
// Rescan was removed for node wallets: liboyoltc owns its UTXO mirror
// and exposes a deterministic rescan op (scantxoutset on the node);
// node-wallet rescan is the node's job — if it falls behind that's a
// node-side problem, not something we paper over with a button here.
func (a *API) handleWalletRescan(w http.ResponseWriter, r *http.Request) {
	name := a.requireWallet(w, r)
	if name == "" {
		return
	}
	ext, isExt := a.wallets.Get(name)
	if !isExt {
		a.jsonError(w, "rescan only supported for OYO wallets — node wallets sync via the node", 501)
		return
	}
	action := r.URL.Query().Get("action")
	if r.Method == "GET" {
		a.jsonResponse(w, map[string]interface{}{"scanning": false})
		return
	}
	if r.Method == "POST" && action == "abort" {
		a.jsonResponse(w, map[string]string{"status": "aborted"})
		return
	}
	if r.Method == "POST" && (action == "start" || action == "") {
		// Pull the chain mirror up to the node's tip first. Without
		// this, scantxoutset hands back UTXOs at heights the wallet's
		// mirror hasn't caught up to yet, and the wallet drops them
		// (the mirror's "what block confirmed this" lookup misses).
		// Caller-visible symptom was a flake where `mine + rescan`
		// returned confirmed_sat=0 on a freshly-funded recipient.
		// SyncOnce is idempotent — no-op if already at tip.
		if _, syncErr := a.wallets.SyncOnce(); syncErr != nil {
			a.jsonError(w, fmt.Sprintf("rescan: chain sync failed: %v", syncErr), 502)
			return
		}
		result, err := runWalletOp(a.rpc, ext.RescanOp())
		if err != nil {
			a.jsonError(w, fmt.Sprintf("%v: %s", err, string(result)), 502)
			return
		}
		a.jsonResponse(w, map[string]interface{}{"status": "rescanned", "result": json.RawMessage(result)})
		return
	}
	a.jsonError(w, "unsupported method/action", 405)
}

// POST /api/mine?count=1&address=...
// GET /api/syncing — aggregated sync / state snapshot.
//
// Returns three sections:
//
//	node      — node-side blockchain (getblockchaininfo + getnetworkinfo
//	            + getindexinfo); height, headers, verification progress,
//	            peers, txindex sync status.
//	oyoltc    — liboyoltc chain status: tip_height, blocks_cached,
//	            addresses_tracked, mempool_size, mweb_pending counts,
//	            utxo_mirror.{regular:{total,unspent}, mweb:{total,unspent}, synced_height}.
//	wallets   — { count, by_kind: {regular, mweb, universal, ...},
//	            total_addresses, total_utxos, total_balance_sat }.
//
// Backs the "Syncing" UI tab — single endpoint, refreshed on a 5s
// tick alongside the rest of the periodic header info.
func (a *API) handleSyncing(w http.ResponseWriter, r *http.Request) {
	out := map[string]interface{}{}

	// Node-side: blockchain + network + txindex.
	nodeOut := map[string]interface{}{}
	if chain, err := a.rpc.CallRaw("getblockchaininfo", nil); err == nil {
		nodeOut["blockchain"] = json.RawMessage(chain)
	}
	if net, err := a.rpc.CallRaw("getnetworkinfo", nil); err == nil {
		nodeOut["network"] = json.RawMessage(net)
	}
	if idx, err := a.rpc.CallRaw("getindexinfo", nil); err == nil {
		nodeOut["index"] = json.RawMessage(idx)
	}
	if mp, err := a.rpc.CallRaw("getmempoolinfo", nil); err == nil {
		nodeOut["mempool"] = json.RawMessage(mp)
	}
	out["node"] = nodeOut
	if a.rpc != nil {
		out["rpc_profile"] = a.rpc.StatsSnapshot()
	}

	// liboyoltc-side: chain.StatusJSON() carries tip / mempool / mweb-mirror.
	// Merge in the registry-level desync flag so the frontend can banner
	// the operator without needing a second roundtrip.
	if a.wallets != nil {
		if status, err := a.wallets.ChainStatusJSON(); err == nil {
			var enriched map[string]interface{}
			_ = json.Unmarshal(status, &enriched)
			if enriched == nil {
				enriched = map[string]interface{}{}
			}
			desync, reason := a.wallets.DesyncStatus()
			enriched["desync"] = desync
			if reason != "" {
				enriched["desync_reason"] = reason
			}
			enriched["sync_prefetch"] = a.wallets.SyncPrefetchStats()
			out["oyoltc"] = enriched
		}
	}

	// Wallets summary: count + breakdown by kind + aggregate balance/utxo.
	type walletEntry struct {
		Name         string `json:"name"`
		Kind         string `json:"kind"`
		Loaded       bool   `json:"loaded"`
		AddressCount int    `json:"address_count"`
		UtxoCount    int    `json:"utxo_count"`
		ConfirmedSat int64  `json:"confirmed_sat"`
		PendingInSat int64  `json:"pending_in_sat"`
	}
	wsumm := map[string]interface{}{}
	if a.wallets != nil {
		all := a.wallets.List()
		byKind := map[string]int{}
		var totalAddr, totalUtxo int
		var totalConfirmed, totalPendingIn int64
		entries := make([]walletEntry, 0, len(all))
		for _, w := range all {
			byKind[w.Kind]++
			var s struct {
				AddressCount int   `json:"address_count"`
				ConfirmedSat int64 `json:"confirmed_sat"`
				PendingInSat int64 `json:"pending_in_sat"`
				UtxoCount    int   `json:"utxo_count"`
			}
			_ = json.Unmarshal(w.StatusJSON(), &s)
			totalAddr += s.AddressCount
			totalUtxo += s.UtxoCount
			totalConfirmed += s.ConfirmedSat
			totalPendingIn += s.PendingInSat
			entries = append(entries, walletEntry{
				Name:         w.Name,
				Kind:         w.Kind,
				Loaded:       true, // List() returns only loaded wallets
				AddressCount: s.AddressCount,
				UtxoCount:    s.UtxoCount,
				ConfirmedSat: s.ConfirmedSat,
				PendingInSat: s.PendingInSat,
			})
		}
		wsumm["count"] = len(all)
		wsumm["by_kind"] = byKind
		wsumm["total_addresses"] = totalAddr
		wsumm["total_utxos"] = totalUtxo
		wsumm["total_confirmed_sat"] = totalConfirmed
		wsumm["total_pending_in_sat"] = totalPendingIn
		wsumm["entries"] = entries
	}
	out["wallets"] = wsumm

	a.jsonResponse(w, out)
}

// POST /api/syncing/resync — wipe the on-disk chain mirror and rebuild
// chain state in-process. Wallet seeds and the registry survive; the
// dropped state is the chain mirror (LevelDB + in-memory blocks +
// mempool cache) and per-wallet UTXO bookkeeping. After Resync each
// wallet has been reopened and its bootstrap op driven against the
// node's current tip.
func (a *API) handleResync(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}
	if a.wallets == nil {
		a.jsonError(w, "wallets registry not initialized", 500)
		return
	}
	res, err := a.wallets.Resync()
	if err != nil {
		a.jsonError(w, "resync failed: "+err.Error(), 500)
		return
	}
	log.Printf("Resync: %v files removed, wallets reopened: %v", res["removed_files"], res["wallets_reopened"])
	res["status"] = "ok"
	a.jsonResponse(w, res)
}

func (a *API) handleMine(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		a.jsonError(w, "POST required", 405)
		return
	}

	count := 1
	if c := r.URL.Query().Get("count"); c != "" {
		n, err := strconv.Atoi(c)
		if err != nil || n < 1 {
			a.jsonError(w, "invalid count", 400)
			return
		}
		if n > 10000 {
			a.jsonError(w, "count too large (max 10000)", 400)
			return
		}
		count = n
	}

	address := r.URL.Query().Get("address")
	if address == "" {
		// Need a wallet to get an address - use first loaded wallet
		walletsRaw, _ := a.rpc.CallRaw("listwallets", nil)
		var wallets []string
		json.Unmarshal(walletsRaw, &wallets)

		if len(wallets) == 0 {
			a.jsonError(w, "no wallets loaded, provide an address", 400)
			return
		}

		// Find a wallet that can generate addresses (has private keys)
		var addrErr error
		for _, wName := range wallets {
			addrRaw, err := a.rpc.CallWallet(wName, "getnewaddress", nil)
			if err == nil {
				json.Unmarshal(addrRaw, &address)
				break
			}
			addrErr = err
		}
		if address == "" {
			a.jsonError(w, "no wallet with keys available: "+addrErr.Error(), 400)
			return
		}
	}

	result, err := a.rpc.CallRaw("generatetoaddress", []interface{}{count, address})
	if err != nil {
		a.jsonError(w, err.Error(), 502)
		return
	}

	log.Printf("Mined %d block(s) to %s", count, address)
	if a.wallets != nil {
		a.wallets.WakeTailer()
	}
	a.jsonResponse(w, map[string]interface{}{
		"address": address,
		"blocks":  result,
	})
}
