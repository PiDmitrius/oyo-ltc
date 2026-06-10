// Package oyoltc is a thin Go wrapper over the liboyoltc C ABI: chain sync,
// wallet UTXO tracking, balances, rescan, send. Go here is pure transport
// and cgo plumbing — all state lives in the C library.
package oyoltc

/*
#cgo CFLAGS: -I${SRCDIR}/../../liboyoltc/include
#cgo LDFLAGS: ${SRCDIR}/../../liboyoltc/lib/oyoltc-stubs.o -L${SRCDIR}/../../liboyoltc/lib -l:liboyoltc_bundle.a -l:libboost_filesystem.a -l:libboost_thread.a -l:libboost_system.a -l:libfmt.a -lpthread -ldl -lstdc++ -l:libcrypto.a -lm

#include "oyoltc.h"
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"unsafe"
)

const (
	OpDone    = int32(C.OYO_OP_DONE)
	OpNeedRPC = int32(C.OYO_OP_NEED_RPC)
	OpFailed  = int32(C.OYO_OP_FAILED)
)

// Wallet-kind defines, mirrored from oyoltc.h. Passed to Chain.Open to
// pick the derivation chain and which wallet sides get pre-allocated.
const (
	KindRegular   = int32(C.OYO_WALLET_KIND_REGULAR)
	KindMweb      = int32(C.OYO_WALLET_KIND_MWEB)
	KindUniversal = int32(C.OYO_WALLET_KIND_UNIVERSAL)
	KindWatch     = int32(C.OYO_WALLET_KIND_WATCH)
)

// Address-side selector for Wallet.NewAddress. AUTO is fine for
// single-side wallets (regular/mweb); universal must pass P2WPKH or MWEB.
const (
	AddrAuto       = int32(C.OYO_ADDR_AUTO)
	AddrP2wpkh     = int32(C.OYO_ADDR_P2WPKH)
	AddrMweb       = int32(C.OYO_ADDR_MWEB)
	AddrP2pkh      = int32(C.OYO_ADDR_P2PKH)       // legacy
	AddrP2shP2wpkh = int32(C.OYO_ADDR_P2SH_P2WPKH) // nested SegWit
)

type Context struct{ h C.OYO_CTX }
type Chain struct {
	h   C.OYO_CHAIN
	ctx *Context
}
type Wallet struct {
	h     C.OYO_WALLET
	chain *Chain
}
type Op struct {
	h   C.OYO_OP
	ctx *Context
}

// Version returns the ABI version baked into liboyoltc.
func Version() uint32 { return uint32(C.oyo_version()) }

// Open creates a new ctx. workdir is the filesystem path for per-chain
// mirrors. Pass an empty string to run in volatile dev/test mode. The
// directory is auto-created if missing.
func Open(workdir string) (*Context, error) {
	var h C.OYO_CTX
	var cwd *C.char
	if workdir != "" {
		cwd = C.CString(workdir)
		defer C.free(unsafe.Pointer(cwd))
	}
	if rc := C.oyo_open(cwd, &h); rc != C.OYO_OK {
		return nil, fmt.Errorf("oyo_open: rc=%d", rc)
	}
	return &Context{h: h}, nil
}

func (c *Context) Close() error {
	if c == nil || c.h == nil {
		return nil
	}
	rc := C.oyo_close(c.h)
	c.h = nil
	if rc != C.OYO_OK {
		return fmt.Errorf("oyo_close: rc=%d", rc)
	}
	return nil
}

func (c *Context) lastErr(tag string, rc C.int32_t) error {
	var code C.int32_t
	var msg *C.uint8_t
	var mlen C.size_t
	C.oyo_last_error(c.h, &code, &msg, &mlen)
	if mlen > 0 {
		return fmt.Errorf("%s: rc=%d: %s", tag, rc, C.GoStringN((*C.char)(unsafe.Pointer(msg)), C.int(mlen)))
	}
	return fmt.Errorf("%s: rc=%d", tag, rc)
}

func ptrLen(b []byte) (*C.uint8_t, C.size_t) {
	if len(b) == 0 {
		return nil, 0
	}
	return (*C.uint8_t)(unsafe.Pointer(&b[0])), C.size_t(len(b))
}

func borrow(p *C.uint8_t, plen C.size_t) []byte {
	if p == nil || plen == 0 {
		return nil
	}
	return C.GoBytes(unsafe.Pointer(p), C.int(plen))
}

// ChainOpen initialises a chain context with the given config JSON.
// cfgJSON may be nil; defaults (regtest, rollback_window=100) apply.
func (c *Context) ChainOpen(cfgJSON []byte) (*Chain, error) {
	p, plen := ptrLen(cfgJSON)
	var h C.OYO_CHAIN
	if rc := C.oyo_chain_open(c.h, p, plen, &h); rc != C.OYO_OK {
		return nil, c.lastErr("oyo_chain_open", rc)
	}
	return &Chain{h: h, ctx: c}, nil
}

func (ch *Chain) Close() error {
	if ch == nil || ch.h == nil {
		return nil
	}
	rc := C.oyo_chain_close(ch.h)
	ch.h = nil
	if rc != C.OYO_OK {
		return fmt.Errorf("oyo_chain_close: rc=%d", rc)
	}
	return nil
}

func (ch *Chain) StatusJSON() (json.RawMessage, error) {
	var p *C.uint8_t
	var plen C.size_t
	if rc := C.oyo_chain_status(ch.h, &p, &plen); rc != C.OYO_OK {
		return nil, ch.ctx.lastErr("oyo_chain_status", rc)
	}
	return json.RawMessage(borrow(p, plen)), nil
}

// AddressStatus computes the confirmed balance, immature split and unspent set
// for an arbitrary canonical address from the local regular-UTXO mirror — no
// node query. reqJSON is {"address":"..."} or {"script_hex":"..."}; see the
// oyo_chain_address_status comment in oyoltc.h for the result shape.
func (ch *Chain) AddressStatus(reqJSON []byte) (json.RawMessage, error) {
	p, plen := ptrLen(reqJSON)
	var op *C.uint8_t
	var oplen C.size_t
	if rc := C.oyo_chain_address_status(ch.h, p, plen, &op, &oplen); rc != C.OYO_OK {
		return nil, ch.ctx.lastErr("oyo_chain_address_status", rc)
	}
	return json.RawMessage(borrow(op, oplen)), nil
}

// OpenWallet creates a wallet of the given kind. cfgJSON shape depends on
// kind — see oyoltc.h for the full schema. Common fields: {"name":...}.
//
//   - KindRegular   {"name", "seed", "address_count"?}
//   - KindMweb      {"name", "seed_type":"oyo_mweb_v1"|"mweb_v0", ...}
//   - KindUniversal {"name", "seed", "kinds"?, "address_count"?}
//   - KindWatch     {"name"}
func (ch *Chain) OpenWallet(kind int32, cfgJSON []byte) (*Wallet, error) {
	p, plen := ptrLen(cfgJSON)
	var h C.OYO_WALLET
	if rc := C.oyo_wallet_open(ch.h, C.int32_t(kind), p, plen, &h); rc != C.OYO_OK {
		return nil, ch.ctx.lastErr("oyo_wallet_open", rc)
	}
	return &Wallet{h: h, chain: ch}, nil
}

// NewAddress allocates the next derived address. addrKind selects the side:
// AddrAuto for single-side wallets, or AddrP2wpkh / AddrMweb for universal.
// Returns (binding_index, encoded_address, err).
func (w *Wallet) NewAddress(addrKind int32) (int32, string, error) {
	var idx C.int32_t
	var p *C.uint8_t
	var plen C.size_t
	rc := C.oyo_wallet_new_address(w.h, C.int32_t(addrKind), &idx, &p, &plen)
	if rc != C.OYO_OK {
		return -1, "", w.chain.ctx.lastErr("oyo_wallet_new_address", rc)
	}
	return int32(idx), string(borrow(p, plen)), nil
}

// AddAddress imports an externally-owned address on a watch-only wallet.
// Returns the new binding index. Errors with UNSUPPORTED on non-watch wallets.
func (w *Wallet) AddAddress(address string) (int32, error) {
	if address == "" {
		return -1, fmt.Errorf("address required")
	}
	b := []byte(address)
	var idx C.int32_t
	rc := C.oyo_wallet_add_address(w.h, (*C.uint8_t)(unsafe.Pointer(&b[0])), C.size_t(len(b)), &idx)
	if rc != C.OYO_OK {
		return -1, w.chain.ctx.lastErr("oyo_wallet_add_address", rc)
	}
	return int32(idx), nil
}

func (w *Wallet) RemoveAddress(bindingIndex int32) error {
	rc := C.oyo_wallet_remove_address(w.h, C.int32_t(bindingIndex))
	if rc != C.OYO_OK {
		return w.chain.ctx.lastErr("oyo_wallet_remove_address", rc)
	}
	return nil
}

// Bootstrap walks the chain's persistent MWEB mirror and registers every
// output that this wallet's keychain can rewind. No-op (returns a Done op
// with status=skipped) for wallets without an MWEB side.
func (w *Wallet) Bootstrap() (*Op, error) {
	var h C.OYO_OP
	if rc := C.oyo_wallet_bootstrap(w.h, &h); rc != C.OYO_OK {
		return nil, w.chain.ctx.lastErr("oyo_wallet_bootstrap", rc)
	}
	return &Op{h: h, ctx: w.chain.ctx}, nil
}

// Send returns an op that builds + signs + broadcasts a transaction.
// Internal dispatch picks regular / mweb / pegin based on wallet kind +
// destination class. cfg JSON shape:
//
//	{"to":"<address>",
//	 "amount_sat":<int> | "send_all":true,
//	 "fee_rate_sat_per_vb":<int>?,
//	 "dry_run":true?}
//
// On op completion the result includes status, txid, amount_sat, fee
// breakdown, change breakdown, inputs_total_sat, inputs_count.
func (w *Wallet) Send(cfgJSON []byte) (*Op, error) {
	if len(cfgJSON) == 0 {
		return nil, fmt.Errorf("send config required")
	}
	p, plen := ptrLen(cfgJSON)
	var h C.OYO_OP
	if rc := C.oyo_wallet_send(w.h, p, plen, &h); rc != C.OYO_OK {
		return nil, w.chain.ctx.lastErr("oyo_wallet_send", rc)
	}
	return &Op{h: h, ctx: w.chain.ctx}, nil
}

func (w *Wallet) Close() error {
	if w == nil || w.h == nil {
		return nil
	}
	rc := C.oyo_wallet_close(w.h)
	w.h = nil
	if rc != C.OYO_OK {
		return fmt.Errorf("oyo_wallet_close: rc=%d", rc)
	}
	return nil
}

// Revision returns the current wallet revision. Monotonically increases on
// any state change affecting this wallet.
func (w *Wallet) Revision() (uint64, error) {
	var rev C.uint64_t
	if rc := C.oyo_wallet_revision(w.h, &rev); rc != C.OYO_OK {
		return 0, fmt.Errorf("oyo_wallet_revision: rc=%d", rc)
	}
	return uint64(rev), nil
}

// StatusSince returns (currentRevision, statusJSON, changed, error).
// If sinceRev == currentRevision the wallet has not changed and changed=false;
// statusJSON is nil and no serialization happens inside the library.
func (w *Wallet) StatusSince(sinceRev uint64) (uint64, json.RawMessage, bool, error) {
	var rev C.uint64_t
	var p *C.uint8_t
	var plen C.size_t
	rc := C.oyo_wallet_status_since(w.h, C.uint64_t(sinceRev), &rev, &p, &plen)
	if rc == C.OYO_NO_CHANGE {
		return uint64(rev), nil, false, nil
	}
	if rc != C.OYO_OK {
		return 0, nil, false, w.chain.ctx.lastErr("oyo_wallet_status_since", rc)
	}
	return uint64(rev), json.RawMessage(borrow(p, plen)), true, nil
}

// ExportSecrets returns the wallet's secret material (seed-derived keys) as
// a JSON blob: {seed_type, p2wpkh:[{index,address,wif}], mweb:{scan_secret,
// spend_secret, addresses}}. DANGEROUS — exposes full spend authority; the
// HTTP layer gates access. Errors with UNSUPPORTED for watch-only wallets.
func (w *Wallet) ExportSecrets() (json.RawMessage, error) {
	var p *C.uint8_t
	var plen C.size_t
	rc := C.oyo_wallet_export_secrets(w.h, &p, &plen)
	if rc != C.OYO_OK {
		return nil, w.chain.ctx.lastErr("oyo_wallet_export_secrets", rc)
	}
	return json.RawMessage(borrow(p, plen)), nil
}

func (ch *Chain) Sync() (*Op, error) {
	var h C.OYO_OP
	if rc := C.oyo_chain_sync(ch.h, &h); rc != C.OYO_OK {
		return nil, ch.ctx.lastErr("oyo_chain_sync", rc)
	}
	return &Op{h: h, ctx: ch.ctx}, nil
}

// MempoolSync drives one full mempool sync pass: getrawmempool + per-txid
// getrawtransaction loop. No-op (Done immediately) if track_mempool was not
// enabled in the chain config.
func (ch *Chain) MempoolSync() (*Op, error) {
	var h C.OYO_OP
	if rc := C.oyo_mempool_sync(ch.h, &h); rc != C.OYO_OK {
		return nil, ch.ctx.lastErr("oyo_mempool_sync", rc)
	}
	return &Op{h: h, ctx: ch.ctx}, nil
}

func (w *Wallet) Rescan() (*Op, error) {
	var h C.OYO_OP
	if rc := C.oyo_wallet_rescan(w.h, &h); rc != C.OYO_OK {
		return nil, w.chain.ctx.lastErr("oyo_wallet_rescan", rc)
	}
	return &Op{h: h, ctx: w.chain.ctx}, nil
}

func (w *Wallet) RescanAddress(bindingIndex int32) (*Op, error) {
	var h C.OYO_OP
	if rc := C.oyo_wallet_rescan_address(w.h, C.int32_t(bindingIndex), &h); rc != C.OYO_OK {
		return nil, w.chain.ctx.lastErr("oyo_wallet_rescan_address", rc)
	}
	return &Op{h: h, ctx: w.chain.ctx}, nil
}

func (op *Op) State() (int32, error) {
	var s C.int32_t
	if rc := C.oyo_op_state(op.h, &s); rc != C.OYO_OK {
		return 0, fmt.Errorf("oyo_op_state: rc=%d", rc)
	}
	return int32(s), nil
}

func (op *Op) RPCRequest() ([]byte, error) {
	var p *C.uint8_t
	var plen C.size_t
	if rc := C.oyo_op_rpc_request(op.h, &p, &plen); rc != C.OYO_OK {
		return nil, fmt.Errorf("oyo_op_rpc_request: rc=%d", rc)
	}
	return borrow(p, plen), nil
}

func (op *Op) ProvideRPCResponse(resp []byte) error {
	p, plen := ptrLen(resp)
	rc := C.oyo_op_provide_rpc_response(op.h, p, plen)
	if rc == C.OYO_OK {
		return nil
	}
	return fmt.Errorf("oyo_op_provide_rpc_response: rc=%d", rc)
}

func (op *Op) Result() (json.RawMessage, error) {
	var p *C.uint8_t
	var plen C.size_t
	if rc := C.oyo_op_result(op.h, &p, &plen); rc != C.OYO_OK {
		return nil, fmt.Errorf("oyo_op_result: rc=%d", rc)
	}
	return json.RawMessage(borrow(p, plen)), nil
}

func (op *Op) Close() error {
	if op == nil || op.h == nil {
		return nil
	}
	rc := C.oyo_op_close(op.h)
	op.h = nil
	if rc != C.OYO_OK {
		return fmt.Errorf("oyo_op_close: rc=%d", rc)
	}
	return nil
}

// RPCFunc sends a single JSON-RPC request body and returns the raw response body.
type RPCFunc func(req []byte) ([]byte, error)

// RunOp drives op to completion through rpc. Final result JSON is returned.
// Caller still owns the op and must Close it.
func RunOp(op *Op, rpc RPCFunc) (json.RawMessage, error) {
	for {
		st, err := op.State()
		if err != nil {
			return nil, err
		}
		switch st {
		case OpDone:
			return op.Result()
		case OpFailed:
			res, _ := op.Result()
			return res, errors.New("op failed")
		case OpNeedRPC:
			req, err := op.RPCRequest()
			if err != nil {
				return nil, err
			}
			resp, err := rpc(req)
			if err != nil {
				return nil, err
			}
			if err := op.ProvideRPCResponse(resp); err != nil {
				res, _ := op.Result()
				return res, err
			}
		default:
			return nil, fmt.Errorf("unknown op state %d", st)
		}
	}
}

// MwebSelfTest exercises the libmw linkage from the bundle: derives a
// stealth address from a fixed test seed and returns the result JSON.
// Used by tests to confirm that MWEB crypto symbols are reachable from cgo.
func (c *Context) MwebSelfTest() (json.RawMessage, error) {
	var p *C.uint8_t
	var plen C.size_t
	if rc := C.oyo_mweb_self_test(c.h, &p, &plen); rc != C.OYO_OK {
		return nil, fmt.Errorf("oyo_mweb_self_test: rc=%d", rc)
	}
	return json.RawMessage(borrow(p, plen)), nil
}
