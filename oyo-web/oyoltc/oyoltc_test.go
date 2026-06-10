package oyoltc

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

func TestVersion(t *testing.T) {
	if v := Version(); v == 0 {
		t.Fatalf("unexpected version %d", v)
	}
}

func TestMwebWalletOpenAndDeriveAddress(t *testing.T) {
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	chain, err := ctx.ChainOpen([]byte(`{"network":"regtest","rollback_window":100}`))
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()

	seed := strings.Repeat("ab", 32) // 64 hex chars
	cfg := []byte(`{"name":"mweb1","seed_type":"oyo_mweb_v1","seed":"` + seed + `"}`)
	w, err := chain.OpenWallet(KindMweb, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	idx0, addr0, err := w.NewAddress(AddrMweb)
	if err != nil {
		t.Fatal(err)
	}
	if idx0 != 0 {
		t.Fatalf("first index = %d, want 0", idx0)
	}
	if !strings.HasPrefix(addr0, "rmweb1") && !strings.HasPrefix(addr0, "tmweb1") {
		t.Fatalf("expected regtest mweb HRP prefix, got %q", addr0)
	}

	// Determinism: opening a fresh wallet with the same seed must yield the
	// same address at index 0.
	w2, err := chain.OpenWallet(KindMweb, []byte(`{"name":"mweb2","seed_type":"oyo_mweb_v1","seed":"` + seed + `"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer w2.Close()
	_, addr0b, err := w2.NewAddress(AddrMweb)
	if err != nil {
		t.Fatal(err)
	}
	if addr0 != addr0b {
		t.Fatalf("non-deterministic derivation:\n  a=%s\n  b=%s", addr0, addr0b)
	}

	// Indices monotonically increase, addresses differ.
	idx1, addr1, err := w.NewAddress(AddrMweb)
	if err != nil {
		t.Fatal(err)
	}
	if idx1 != 1 {
		t.Fatalf("second index = %d, want 1", idx1)
	}
	if addr0 == addr1 {
		t.Fatalf("index-0 and index-1 produced same address: %s", addr0)
	}

	// Wallet status JSON exposes mweb section with both addresses.
	_, status, _, err := w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	var s struct {
		Mweb *struct {
			Kind         string `json:"kind"`
			AddressCount int64  `json:"address_count"`
			BalanceSat   int64  `json:"balance_sat"`
			Addresses    []struct {
				Index   int    `json:"index"`
				Address string `json:"address"`
			} `json:"addresses"`
		} `json:"mweb"`
	}
	if err := json.Unmarshal(status, &s); err != nil {
		t.Fatalf("decode status: %v (raw=%s)", err, string(status))
	}
	if s.Mweb == nil || s.Mweb.Kind != "mweb" {
		t.Fatalf("status missing mweb section: %s", string(status))
	}
	if s.Mweb.AddressCount != 2 || len(s.Mweb.Addresses) != 2 {
		t.Fatalf("address_count=%d, len=%d, want 2", s.Mweb.AddressCount, len(s.Mweb.Addresses))
	}
	if s.Mweb.BalanceSat != 0 {
		t.Fatalf("fresh wallet balance_sat = %d, want 0", s.Mweb.BalanceSat)
	}
	if s.Mweb.Addresses[0].Address != addr0 || s.Mweb.Addresses[1].Address != addr1 {
		t.Fatalf("status address mismatch:\n  expected: %q, %q\n  got:      %q, %q",
			addr0, addr1, s.Mweb.Addresses[0].Address, s.Mweb.Addresses[1].Address)
	}
}

func TestMwebWalletRejectsInvalidSeed(t *testing.T) {
	ctx, _ := Open("")
	defer ctx.Close()
	chain, _ := ctx.ChainOpen([]byte(`{"network":"regtest"}`))
	defer chain.Close()

	// After seed unification, MWEB wallets accept any non-empty string —
	// liboyoltc hashes it via the oyo_mweb_v1_seed domain to derive the
	// 32-byte master. Only empty seeds and unsupported seed_types fail.
	bad := [][]byte{
		[]byte(`{"name":"x","seed_type":"oyo_mweb_v1"}`),                       // missing seed
		[]byte(`{"name":"x","seed_type":"oyo_mweb_v1","seed":""}`),              // empty seed
		[]byte(`{"name":"x","seed_type":"unknown","seed":"anything"}`),         // unsupported seed_type
	}
	for i, cfg := range bad {
		if _, err := chain.OpenWallet(KindMweb, cfg); err == nil {
			t.Errorf("case %d: expected error for %s", i, string(cfg))
		}
	}
}

func TestMwebSelfTest(t *testing.T) {
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()

	raw, err := ctx.MwebSelfTest()
	if err != nil {
		t.Fatal(err)
	}
	var got struct {
		Status      string `json:"status"`
		ScanPubkey  string `json:"scan_pubkey"`
		SpendPubkey string `json:"spend_pubkey"`
		StealthA    string `json:"stealth_a"`
		StealthB    string `json:"stealth_b"`
	}
	if err := json.Unmarshal(raw, &got); err != nil {
		t.Fatalf("decode: %v (raw=%s)", err, string(raw))
	}
	if got.Status != "ok" {
		t.Fatalf("status=%q, want ok (raw=%s)", got.Status, string(raw))
	}
	for name, v := range map[string]string{
		"scan_pubkey":  got.ScanPubkey,
		"spend_pubkey": got.SpendPubkey,
		"stealth_a":    got.StealthA,
		"stealth_b":    got.StealthB,
	} {
		if len(v) != 66 {
			t.Errorf("%s: hex len=%d, want 66 (compressed pubkey, val=%q)", name, len(v), v)
		}
		if !strings.HasPrefix(v, "02") && !strings.HasPrefix(v, "03") {
			t.Errorf("%s: prefix=%q, want compressed pubkey 02/03", name, v[:2])
		}
	}

	// Determinism: second call must produce the same JSON.
	raw2, err := ctx.MwebSelfTest()
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != string(raw2) {
		t.Fatalf("non-deterministic self-test:\n  a=%s\n  b=%s", string(raw), string(raw2))
	}
}

func TestOpenCloseWalletLifecycle(t *testing.T) {
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()

	chain, err := ctx.ChainOpen([]byte(`{"network":"regtest","rollback_window":100}`))
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()

	wcfg := []byte(`{"name":"probe","seed_type":"oyo_v1","seed":"probe-seed","address_count":3,"address_kind":"p2wpkh"}`)
	w, err := chain.OpenWallet(KindRegular, wcfg)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Fresh wallet: revision > 0 (attach-watcher bumps per address).
	rev0, err := w.Revision()
	if err != nil {
		t.Fatal(err)
	}
	if rev0 == 0 {
		t.Fatal("expected non-zero initial revision")
	}

	rev, status, changed, err := w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	if !changed {
		t.Fatal("expected changed=true for sinceRev=0")
	}
	if rev != rev0 {
		t.Fatalf("rev=%d want %d", rev, rev0)
	}
	var parsed struct {
		Name             string  `json:"name"`
		Revision         uint64  `json:"revision"`
		Desync           bool    `json:"desync"`
		DesyncAddresses  []int   `json:"desync_addresses"`
		AddressCount     int     `json:"address_count"`
		ConfirmedSat     int64   `json:"confirmed_sat"`
	}
	if err := json.Unmarshal(status, &parsed); err != nil {
		t.Fatalf("bad status JSON: %v\n%s", err, string(status))
	}
	if parsed.Name != "probe" || parsed.AddressCount != 3 || parsed.Revision != rev {
		t.Fatalf("unexpected status: %+v", parsed)
	}
	if !parsed.Desync || len(parsed.DesyncAddresses) != 3 {
		t.Fatalf("expected all-desync fresh wallet: %+v", parsed)
	}
	if parsed.ConfirmedSat != 0 {
		t.Fatalf("expected zero balance: %+v", parsed)
	}

	// Same revision: NO_CHANGE path — changed=false, status=nil.
	rev2, status2, changed2, err := w.StatusSince(rev)
	if err != nil {
		t.Fatal(err)
	}
	if changed2 {
		t.Fatal("expected NO_CHANGE path")
	}
	if status2 != nil {
		t.Fatal("expected nil status on NO_CHANGE")
	}
	if rev2 != rev {
		t.Fatalf("rev2=%d want %d", rev2, rev)
	}

	// chain_sync produces a first NEED_RPC request we can inspect without a real node.
	op, err := chain.Sync()
	if err != nil {
		t.Fatal(err)
	}
	defer op.Close()
	state, err := op.State()
	if err != nil {
		t.Fatal(err)
	}
	if state != OpNeedRPC {
		t.Fatalf("state=%d want NEED_RPC", state)
	}
	req, err := op.RPCRequest()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(req), `"method":"getblockchaininfo"`) {
		t.Fatalf("unexpected first rpc: %s", string(req))
	}
}

func TestDerivedAddressesMatchLegacy(t *testing.T) {
	// Locked outputs for seed="probe-seed" on regtest (HRP=rltc).
	// Same priv-key derivation as the old Go prototype, but bech32 HRP fixed
	// to match the litecoind regtest network.
	want := []string{
		"rltc1q2rserqdkszkcl9g2c766m07g3n2yxgy2nct0zw",
		"rltc1q9pau66ypl63grn360yqv8svvttkum4p8jw0ced",
		"rltc1qe3ec7ael67yc90py08urqztqm54myr2nq8hfda",
	}
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	chain, err := ctx.ChainOpen([]byte(`{"network":"regtest"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()
	w, err := chain.OpenWallet(KindRegular, []byte(`{"name":"cmp","seed_type":"oyo_v1","seed":"probe-seed","address_count":3,"address_kind":"p2wpkh"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	_, js, _, err := w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	var parsed struct {
		Addresses []struct {
			Address string `json:"address"`
		} `json:"addresses"`
	}
	if err := json.Unmarshal(js, &parsed); err != nil {
		t.Fatal(err)
	}
	if len(parsed.Addresses) != len(want) {
		t.Fatalf("addr count=%d want %d", len(parsed.Addresses), len(want))
	}
	for i := range want {
		if parsed.Addresses[i].Address != want[i] {
			t.Fatalf("addr[%d]=%s want %s", i, parsed.Addresses[i].Address, want[i])
		}
	}
}

func TestWatchWalletAddRemoveAddress(t *testing.T) {
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	chain, err := ctx.ChainOpen([]byte(`{"network":"regtest"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()

	w, err := chain.OpenWallet(KindWatch, []byte(`{"name":"watch1"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Empty wallet first.
	_, js, _, err := w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	var st struct {
		Type         string `json:"type"`
		WatchOnly    bool   `json:"watch_only"`
		AddressCount int    `json:"address_count"`
		Addresses    []struct {
			Index   int    `json:"index"`
			Address string `json:"address"`
			Kind    string `json:"kind"`
		} `json:"addresses"`
	}
	if err := json.Unmarshal(js, &st); err != nil {
		t.Fatalf("status json: %v", err)
	}
	if st.Type != "watch" || !st.WatchOnly {
		t.Fatalf("expected watch type, got %+v", st)
	}
	if st.AddressCount != 0 {
		t.Fatalf("expected empty wallet, got %d addresses", st.AddressCount)
	}

	// Add two real regtest bech32 P2WPKH addresses derived from a known
	// seed (matches TestDerivedAddressesMatchLegacy). Legacy/P2SH coverage
	// is in the playwright e2e suite — those tests pull real addresses
	// from the live node so we don't have to hand-encode base58 here.
	want := []struct {
		addr string
		kind string
	}{
		{"rltc1q2rserqdkszkcl9g2c766m07g3n2yxgy2nct0zw", "p2wpkh"},
		{"rltc1q9pau66ypl63grn360yqv8svvttkum4p8jw0ced", "p2wpkh"},
	}
	for i, c := range want {
		idx, err := w.AddAddress(c.addr)
		if err != nil {
			t.Fatalf("add #%d (%s): %v", i, c.addr, err)
		}
		if idx != int32(i) {
			t.Fatalf("expected index %d, got %d for %s", i, idx, c.addr)
		}
	}

	// Adding the same address again must error (duplicate-in-wallet).
	if _, err := w.AddAddress(want[0].addr); err == nil {
		t.Fatal("expected duplicate-add error, got nil")
	}

	// Status reflects bindings.
	_, js, _, err = w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(js, &st); err != nil {
		t.Fatal(err)
	}
	if st.AddressCount != 2 {
		t.Fatalf("expected 2 addresses, got %d", st.AddressCount)
	}
	for i, c := range want {
		entry := st.Addresses[i]
		if entry.Index != i || entry.Address != c.addr || entry.Kind != c.kind {
			t.Fatalf("addr[%d] = %+v, want index=%d kind=%s addr=%s", i, entry, i, c.kind, c.addr)
		}
	}

	// Remove index 0 — slot stays at index 0 but becomes a hole.
	if err := w.RemoveAddress(0); err != nil {
		t.Fatal(err)
	}
	_, js, _, _ = w.StatusSince(0)
	if err := json.Unmarshal(js, &st); err != nil {
		t.Fatal(err)
	}
	if st.AddressCount != 1 {
		t.Fatalf("after remove expected 1 address, got %d", st.AddressCount)
	}
	if st.Addresses[0].Index != 1 || st.Addresses[0].Address != want[1].addr {
		t.Fatalf("after remove[0] surviving address should be #1 %s, got %+v", want[1].addr, st.Addresses[0])
	}

	// Re-add #0 — gets a fresh slot at index 2 (slots aren't reused).
	idx2, err := w.AddAddress(want[0].addr)
	if err != nil {
		t.Fatal(err)
	}
	if idx2 != 2 {
		t.Fatalf("re-add expected new index 2, got %d", idx2)
	}

	// Removing an out-of-range index errors.
	if err := w.RemoveAddress(99); err == nil {
		t.Fatal("expected error removing nonexistent slot")
	}
}

func TestWatchWalletRejectsInvalidAddress(t *testing.T) {
	ctx, _ := Open("")
	defer ctx.Close()
	chain, _ := ctx.ChainOpen([]byte(`{"network":"regtest"}`))
	defer chain.Close()
	w, err := chain.OpenWallet(KindWatch, []byte(`{"name":"bad"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	if _, err := w.AddAddress("not an address"); err == nil {
		t.Fatal("expected error on invalid address")
	}
	// Mainnet bech32 on regtest chain — wrong HRP.
	if _, err := w.AddAddress("ltc1q2rserqdkszkcl9g2c766m07g3n2yxgy2k524js"); err == nil {
		t.Fatal("expected error on wrong-network address")
	}
}

func TestSimulatedRescanClearsDesync(t *testing.T) {
	ctx, err := Open("")
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	chain, err := ctx.ChainOpen([]byte(`{"network":"regtest"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()
	w, err := chain.OpenWallet(KindRegular, []byte(`{"name":"rsx","seed_type":"oyo_v1","seed":"probe-seed","address_count":2,"address_kind":"p2wpkh"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	op, err := w.Rescan()
	if err != nil {
		t.Fatal(err)
	}
	defer op.Close()

	// Mirror-rescan is synchronous: it reads the local UTXO mirror, no node
	// scantxoutset round-trip, so the op is Done immediately.
	state, _ := op.State()
	if state != OpDone {
		t.Fatalf("mirror rescan state=%d want Done (no RPC)", state)
	}

	_, js, _, err := w.StatusSince(0)
	if err != nil {
		t.Fatal(err)
	}
	var parsed struct {
		Desync          bool  `json:"desync"`
		DesyncAddresses []int `json:"desync_addresses"`
	}
	if err := json.Unmarshal(js, &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Desync || len(parsed.DesyncAddresses) != 0 {
		t.Fatalf("expected desync cleared after rescan: %+v", parsed)
	}
}

// TestRegularJournalPrune drives a short synthetic chain (no real node) through
// the sync op and checks the Stage-1 prune + Stage-2 format: the reorg journals
// are trimmed to max_reorg_depth, the address index survives below the prune
// floor, meta counters are untouched, and a reopen resumes the tip via the
// u32/range-based BlockTrail::Tip() and the schema-version guard.
func TestRegularJournalPrune(t *testing.T) {
	ctx, err := Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	cfg := []byte(`{"network":"regtest","max_reorg_depth":2,"native_block_parse":false}`)
	chain, err := ctx.ChainOpen(cfg)
	if err != nil {
		t.Fatal(err)
	}

	const N = 8 // heights 0..7 -> prune_floor = 7 - 2 = 5
	script := func(h int) string { return fmt.Sprintf("0014%040x", h+1) } // P2WPKH, 22 bytes
	blkHash := func(h int) string { return fmt.Sprintf("%064x", h+1) }
	txid := func(h int) string { return fmt.Sprintf("%064x", 1000+h) }
	blockJSON := func(h int) string {
		return fmt.Sprintf(`{"height":%d,"hash":"%s","tx":[{"txid":"%s","vin":[{"coinbase":"00"}],`+
			`"vout":[{"n":0,"value":50.0,"scriptPubKey":{"hex":"%s"}}]}]}`,
			h, blkHash(h), txid(h), script(h))
	}
	wrap := func(result string) []byte { return []byte(`{"result":` + result + `,"error":null,"id":1}`) }
	mock := func(req []byte) ([]byte, error) {
		var r struct {
			Method string            `json:"method"`
			Params []json.RawMessage `json:"params"`
		}
		if e := json.Unmarshal(req, &r); e != nil {
			return nil, e
		}
		switch r.Method {
		case "getblockchaininfo":
			return wrap(fmt.Sprintf(`{"chain":"regtest","blocks":%d,"bestblockhash":"%s"}`, N-1, blkHash(N-1))), nil
		case "getblockhash":
			var h int
			json.Unmarshal(r.Params[0], &h)
			return wrap(`"` + blkHash(h) + `"`), nil
		case "getblock":
			var hash string
			json.Unmarshal(r.Params[0], &hash)
			for h := 0; h < N; h++ {
				if blkHash(h) == hash {
					return wrap(blockJSON(h)), nil
				}
			}
			return nil, fmt.Errorf("unknown block hash %s", hash)
		}
		return nil, fmt.Errorf("unexpected rpc method %s", r.Method)
	}

	op, err := chain.Sync()
	if err != nil {
		t.Fatal(err)
	}
	if res, e := RunOp(op, mock); e != nil {
		t.Fatalf("sync failed: %v (%s)", e, res)
	}
	op.Close()

	type status struct {
		TipHeight         int64 `json:"tip_height"`
		RegularPruneFloor int64 `json:"regular_prune_floor"`
		UtxoMirror        struct {
			Regular struct {
				Total   int64 `json:"total"`
				Unspent int64 `json:"unspent"`
			} `json:"regular"`
		} `json:"utxo_mirror"`
	}
	parseStatus := func(ch *Chain) status {
		raw, e := ch.StatusJSON()
		if e != nil {
			t.Fatal(e)
		}
		var s status
		if e := json.Unmarshal(raw, &s); e != nil {
			t.Fatalf("status json: %v (%s)", e, raw)
		}
		return s
	}

	s := parseStatus(chain)
	if s.TipHeight != N-1 {
		t.Fatalf("tip_height=%d want %d", s.TipHeight, N-1)
	}
	if s.RegularPruneFloor != int64(N-1-2) {
		t.Fatalf("regular_prune_floor=%d want %d (prune did not fire)", s.RegularPruneFloor, N-1-2)
	}
	// Prune must NOT touch the meta counters: every output ever is still counted.
	if s.UtxoMirror.Regular.Total != int64(N) {
		t.Fatalf("regular total=%d want %d (prune corrupted counters)", s.UtxoMirror.Regular.Total, N)
	}

	// Below-floor address (height 1 < floor 5): its output-journal row was
	// pruned, but the address index ('a') is full history -> still resolvable.
	as, err := chain.AddressStatus([]byte(`{"script_hex":"` + script(1) + `"}`))
	if err != nil {
		t.Fatalf("address status: %v", err)
	}
	var a struct {
		Utxos []struct {
			Height int64 `json:"height"`
		} `json:"utxos"`
	}
	json.Unmarshal(as, &a)
	if len(a.Utxos) != 1 || a.Utxos[0].Height != 1 {
		t.Fatalf("below-floor address lost from index after prune: %s", as)
	}

	// Reopen: exercises schema-version guard (compatible v3), resume via the
	// range-based u32 BlockTrail::Tip(), and persistence of the prune floor.
	if err := chain.Close(); err != nil {
		t.Fatal(err)
	}
	chain2, err := ctx.ChainOpen(cfg)
	if err != nil {
		t.Fatalf("reopen rejected a compatible mirror: %v", err)
	}
	defer chain2.Close()
	s2 := parseStatus(chain2)
	if s2.TipHeight != N-1 {
		t.Fatalf("reopen tip_height=%d want %d (range Tip/u32 resume broken)", s2.TipHeight, N-1)
	}
	if s2.RegularPruneFloor != int64(N-1-2) {
		t.Fatalf("reopen regular_prune_floor=%d want %d (not persisted)", s2.RegularPruneFloor, N-1-2)
	}
}

// TestAddressHistoryNewestFirst asserts ScanScript walks an address index
// newest-first: both the live UTXO list and the history list come back ordered
// by descending height. That ordering is what makes the kMaxUtxosListed
// truncation keep the most RECENT outputs (older ones are likelier spent).
func TestAddressHistoryNewestFirst(t *testing.T) {
	ctx, err := Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	cfg := []byte(`{"network":"regtest","native_block_parse":false}`)
	chain, err := ctx.ChainOpen(cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer chain.Close()

	const N = 6 // heights 0..5; every block's coinbase pays the SAME address
	const scriptHex = "00140102030405060708090a0b0c0d0e0f1011121314"
	blkHash := func(h int) string { return fmt.Sprintf("%064x", h+1) }
	txid := func(h int) string { return fmt.Sprintf("%064x", 1000+h) }
	blockJSON := func(h int) string {
		return fmt.Sprintf(`{"height":%d,"hash":"%s","tx":[{"txid":"%s","vin":[{"coinbase":"00"}],`+
			`"vout":[{"n":0,"value":50.0,"scriptPubKey":{"hex":"%s"}}]}]}`,
			h, blkHash(h), txid(h), scriptHex)
	}
	wrap := func(result string) []byte { return []byte(`{"result":` + result + `,"error":null,"id":1}`) }
	mock := func(req []byte) ([]byte, error) {
		var r struct {
			Method string            `json:"method"`
			Params []json.RawMessage `json:"params"`
		}
		if e := json.Unmarshal(req, &r); e != nil {
			return nil, e
		}
		switch r.Method {
		case "getblockchaininfo":
			return wrap(fmt.Sprintf(`{"chain":"regtest","blocks":%d,"bestblockhash":"%s"}`, N-1, blkHash(N-1))), nil
		case "getblockhash":
			var h int
			json.Unmarshal(r.Params[0], &h)
			return wrap(`"` + blkHash(h) + `"`), nil
		case "getblock":
			var hash string
			json.Unmarshal(r.Params[0], &hash)
			for h := 0; h < N; h++ {
				if blkHash(h) == hash {
					return wrap(blockJSON(h)), nil
				}
			}
			return nil, fmt.Errorf("unknown block hash %s", hash)
		}
		return nil, fmt.Errorf("unexpected rpc method %s", r.Method)
	}

	op, err := chain.Sync()
	if err != nil {
		t.Fatal(err)
	}
	if res, e := RunOp(op, mock); e != nil {
		t.Fatalf("sync failed: %v (%s)", e, res)
	}
	op.Close()

	as, err := chain.AddressStatus([]byte(`{"script_hex":"` + scriptHex + `","history":true}`))
	if err != nil {
		t.Fatalf("address status: %v", err)
	}
	var a struct {
		TipHeight int64 `json:"tip_height"`
		Utxos []struct {
			Height int64 `json:"height"`
		} `json:"utxos"`
		Received []struct {
			Height int64 `json:"height"`
		} `json:"received"`
	}
	if e := json.Unmarshal(as, &a); e != nil {
		t.Fatalf("status json: %v (%s)", e, as)
	}
	if a.TipHeight != N-1 {
		t.Fatalf("tip_height=%d want %d (drives relative-age UI)", a.TipHeight, N-1)
	}

	// Both lists must hold every output (reverse scan loses nothing) ordered
	// strictly newest-first: heights N-1, N-2, ..., 0.
	check := func(name string, get func(i int) (int64, bool)) {
		for i := 0; i < N; i++ {
			h, ok := get(i)
			if !ok {
				t.Fatalf("%s: only %d entries, want %d (reverse scan dropped rows): %s", name, i, N, as)
			}
			if want := int64(N - 1 - i); h != want {
				t.Fatalf("%s[%d].height=%d want %d (not newest-first): %s", name, i, h, want, as)
			}
		}
	}
	check("utxos", func(i int) (int64, bool) {
		if i >= len(a.Utxos) {
			return 0, false
		}
		return a.Utxos[i].Height, true
	})
	check("received", func(i int) (int64, bool) {
		if i >= len(a.Received) {
			return 0, false
		}
		return a.Received[i].Height, true
	})

	// Explorer scan bound: with limit=3 the walk must stop after the newest 3
	// outputs (heights 5,4,3), flag the view truncated, and NOT scan the rest.
	as3, err := chain.AddressStatus([]byte(`{"script_hex":"` + scriptHex + `","history":true,"limit":3}`))
	if err != nil {
		t.Fatalf("address status (limit): %v", err)
	}
	var a3 struct {
		Utxos []struct {
			Height int64 `json:"height"`
		} `json:"utxos"`
		UtxosTruncated bool `json:"utxos_truncated"`
		ScanLimit int64 `json:"scan_limit"`
		NextLimit int64 `json:"next_limit"`
		Received []struct {
			Height int64 `json:"height"`
		} `json:"received"`
		ReceivedTruncated bool `json:"received_truncated"`
	}
	if e := json.Unmarshal(as3, &a3); e != nil {
		t.Fatalf("status json (limit): %v (%s)", e, as3)
	}
	if len(a3.Received) != 3 || a3.Received[0].Height != 5 || a3.Received[1].Height != 4 || a3.Received[2].Height != 3 {
		t.Fatalf("limit=3 received not newest-3 [5,4,3]: %s", as3)
	}
	if len(a3.Utxos) != 3 {
		t.Fatalf("limit=3 utxos=%d want 3: %s", len(a3.Utxos), as3)
	}
	if !a3.ReceivedTruncated || !a3.UtxosTruncated {
		t.Fatalf("limit=3 over 6 outputs must flag truncated: %s", as3)
	}
	// The engine drives the UI's doubling: scan_limit echoes the window, next_limit = 2x.
	if a3.ScanLimit != 3 || a3.NextLimit != 6 {
		t.Fatalf("limit=3 scan_limit=%d next_limit=%d want 3/6: %s", a3.ScanLimit, a3.NextLimit, as3)
	}

	// No ceiling: a large ?limit is honored verbatim (operator decides depth). With
	// only 6 outputs the window covers them all → not truncated, next_limit 0.
	as5, err := chain.AddressStatus([]byte(`{"script_hex":"` + scriptHex + `","history":true,"limit":5000}`))
	if err != nil {
		t.Fatalf("address status (limit 5000): %v", err)
	}
	var a5 struct {
		ScanLimit int64 `json:"scan_limit"`
		NextLimit int64 `json:"next_limit"`
	}
	if e := json.Unmarshal(as5, &a5); e != nil {
		t.Fatalf("status json (limit 5000): %v (%s)", e, as5)
	}
	if a5.ScanLimit != 5000 || a5.NextLimit != 0 {
		t.Fatalf("limit=5000 scan_limit=%d next_limit=%d want 5000/0 (no clamp): %s", a5.ScanLimit, a5.NextLimit, as5)
	}
}
