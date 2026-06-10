package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"strings"
	"time"
)

// queueTaskBody is the wire shape /api/queue accepts on POST. Mirrors
// the recipe fields of QueueTask + ScheduledAt as a string (the body
// goes through JSON, time.Time would force RFC3339 on callers).
type queueTaskBody struct {
	Wallet      string         `json:"wallet"`
	To          string         `json:"to,omitempty"`
	AmountSat   int64          `json:"amount_sat,omitempty"`
	Outputs     []QueueOutput  `json:"outputs,omitempty"`
	SendAll       bool         `json:"send_all,omitempty"`
	FeeRate       int64        `json:"fee_rate_sat_per_vb,omitempty"`
	Inputs        []QueueInput `json:"inputs,omitempty"`
	ChangeAddress string       `json:"change_address,omitempty"`
	ScheduledAt   string       `json:"scheduled_at,omitempty"`
}

// GET    /api/queue              list all tasks (?wallet= filter)
// POST   /api/queue              add a task (body = queueTaskBody)
func (a *API) handleQueue(w http.ResponseWriter, r *http.Request) {
	if a.queue == nil {
		a.jsonError(w, "queue not available", 503)
		return
	}
	switch r.Method {
	case "GET":
		wallet := r.URL.Query().Get("wallet")
		tasks := a.queue.List(wallet)
		a.jsonResponse(w, tasks)
	case "POST":
		body, err := decodeQueueBody(r)
		if err != nil {
			a.jsonError(w, "invalid body: "+err.Error(), 400)
			return
		}
		scheduled, err := ParseScheduledAt(body.ScheduledAt)
		if err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		t := &QueueTask{
			Wallet:        body.Wallet,
			To:            body.To,
			AmountSat:     body.AmountSat,
			Outputs:       body.Outputs,
			SendAll:       body.SendAll,
			FeeRate:       body.FeeRate,
			Inputs:        body.Inputs,
			ChangeAddress: body.ChangeAddress,
			ScheduledAt:   scheduled,
		}
		// Reject queueing for an unknown wallet upfront so the user
		// sees the error immediately rather than at scheduled-fire.
		if _, ok := a.wallets.Get(body.Wallet); !ok {
			if _, rpcErr := a.rpc.CallWallet(body.Wallet, "getwalletinfo", nil); rpcErr != nil {
				a.jsonError(w, fmt.Sprintf("wallet %q not found", body.Wallet), 404)
				return
			}
		}
		if err := a.queue.Add(t); err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		log.Printf("queue add %s wallet=%s scheduled=%v", t.ID, t.Wallet, t.ScheduledAt)
		a.jsonResponse(w, t)
	default:
		a.jsonError(w, "GET or POST required", 405)
	}
}

// GET    /api/queue/{id}            single-task detail
// POST   /api/queue/{id}/run        execute now
// DELETE /api/queue/{id}            remove
func (a *API) handleQueueItem(w http.ResponseWriter, r *http.Request) {
	if a.queue == nil {
		a.jsonError(w, "queue not available", 503)
		return
	}
	// Path: /api/queue/<id> or /api/queue/<id>/run
	rest := strings.TrimPrefix(r.URL.Path, "/api/queue/")
	parts := strings.Split(rest, "/")
	if len(parts) == 0 || parts[0] == "" {
		a.jsonError(w, "id required", 400)
		return
	}
	id := parts[0]
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}

	switch {
	case r.Method == "GET" && action == "":
		t, ok := a.queue.Get(id)
		if !ok {
			a.jsonError(w, "task not found", 404)
			return
		}
		a.jsonResponse(w, t)

	case r.Method == "POST" && action == "run":
		t, err := a.queue.RunNow(id)
		if err != nil {
			a.jsonError(w, err.Error(), 400)
			return
		}
		a.jsonResponse(w, t)

	case r.Method == "DELETE" && action == "":
		if !a.queue.Remove(id) {
			a.jsonError(w, "task not found", 404)
			return
		}
		a.jsonResponse(w, map[string]string{"removed": id})

	default:
		a.jsonError(w, "method/action not allowed", 405)
	}
}

func decodeQueueBody(r *http.Request) (queueTaskBody, error) {
	var body queueTaskBody
	if r.Body == nil {
		return body, errors.New("body required")
	}
	const maxBodyBytes = 64 * 1024
	raw, err := io.ReadAll(io.LimitReader(r.Body, maxBodyBytes))
	if err != nil {
		return body, err
	}
	if len(raw) == 0 {
		return body, errors.New("body required")
	}
	if err := json.Unmarshal(raw, &body); err != nil {
		return body, err
	}
	return body, nil
}

// ExecuteRecipe satisfies the TaskExecutor interface: builds, signs,
// and broadcasts a tx for the given task's recipe against the current
// wallet state. Used by the queue runner — both manual `/run` and the
// scheduled ticker funnel through here.
//
// For OYO wallets we drive ext.SendOp(cfg) with dry_run=false (single
// roundtrip = build + sign + broadcast inside liboyoltc). For node
// wallets we call the oyo-send RPC directly (which has the same
// semantics on the C++ side via CommitTransaction). Multi-recipient
// and manual inputs are OYO-only at the moment — node-side oyo-send
// only takes a single recipient — so those features are validated at
// queue-add time for node tasks.
func (a *API) ExecuteRecipe(t *QueueTask) (string, error) {
	if t.Wallet == "" {
		return "", errors.New("wallet required")
	}
	if ext, ok := a.wallets.Get(t.Wallet); ok {
		return a.executeOyoRecipe(ext, t)
	}
	return a.executeNodeRecipe(t)
}

func (a *API) executeOyoRecipe(ext *Wallet, t *QueueTask) (string, error) {
	// Sync the chain to tip before coin selection. A chained queue task —
	// one that spends a prior task's just-confirmed change — would otherwise
	// build against a stale UTXO view and fail with "insufficient funds":
	// the tailer is only woken AFTER a send (async), so it can't help the
	// very next task. SyncOnce is idempotent and a near no-op when already
	// at tip, so the cost on non-chained sends is negligible.
	if _, err := a.wallets.SyncOnce(); err != nil {
		log.Printf("queue: pre-send chain sync failed for %s: %v", t.Wallet, err)
	}
	cfg := buildSendCfgFromTask(t, false /* dry_run */)
	op := ext.SendOp(cfg)
	res, opErr := runWalletOp(a.rpc, op)
	if opErr != nil {
		return "", errors.New(extractOpError(res, opErr))
	}
	var parsed struct {
		Txid string `json:"txid"`
	}
	_ = json.Unmarshal(res, &parsed)
	if parsed.Txid == "" {
		return "", fmt.Errorf("tx broadcast but txid missing in result: %s", string(res))
	}
	a.wallets.WakeTailer()
	if a.mempool != nil {
		a.mempool.Refresh()
	}
	return parsed.Txid, nil
}

func (a *API) executeNodeRecipe(t *QueueTask) (string, error) {
	if len(t.Outputs) > 1 {
		return "", errors.New("multi-recipient is not supported for node wallets")
	}
	if len(t.Inputs) > 0 {
		return "", errors.New("manual inputs are not supported for node wallets")
	}
	dest := t.To
	amt := float64(t.AmountSat) / 1e8
	if len(t.Outputs) == 1 {
		dest = t.Outputs[0].Address
		amt = float64(t.Outputs[0].AmountSat) / 1e8
	}
	if dest == "" {
		return "", errors.New("to required")
	}
	if !a.hasOyoFeature("oyo-send") {
		if t.SendAll {
			return "", errors.New("node wallet queued send_all requires oyo-send")
		}
		if t.FeeRate > 0 {
			return "", errors.New("node wallet queued fee_rate requires oyo-send")
		}
		txidRaw, err := a.rpc.CallWallet(t.Wallet, "sendtoaddress", []interface{}{dest, amt})
		if err != nil {
			return "", err
		}
		var txid string
		_ = json.Unmarshal(txidRaw, &txid)
		if txid == "" {
			return "", fmt.Errorf("tx broadcast but txid missing in result: %s", string(txidRaw))
		}
		if a.mempool != nil {
			a.mempool.Refresh()
		}
		if a.wallets != nil {
			a.wallets.WakeTailer()
		}
		return txid, nil
	}
	options := map[string]interface{}{}
	if t.SendAll {
		options["send_all"] = true
	}
	if t.FeeRate > 0 {
		options["fee_rate"] = float64(t.FeeRate) / 1e8
	}
	raw, err := a.rpc.CallWallet(t.Wallet, "oyo-send", []interface{}{dest, amt, options})
	if err != nil {
		return "", err
	}
	var parsed struct {
		TxID string `json:"txid"`
	}
	_ = json.Unmarshal(raw, &parsed)
	if parsed.TxID == "" {
		return "", fmt.Errorf("tx broadcast but txid missing in result: %s", string(raw))
	}
	if a.mempool != nil {
		a.mempool.Refresh()
	}
	if a.wallets != nil {
		a.wallets.WakeTailer()
	}
	return parsed.TxID, nil
}

// buildSendCfgFromTask serializes a QueueTask's recipe into the JSON
// shape liboyoltc's oyo_wallet_send accepts. Mirrors the buildCfg
// closure inside handleWalletSend so the cfg seen by liboyoltc is
// identical whether the caller went through /api/wallet/send or the
// queue runner.
func buildSendCfgFromTask(t *QueueTask, dryRun bool) []byte {
	cfg := map[string]interface{}{}
	if len(t.Outputs) > 0 {
		outs := make([]map[string]interface{}, len(t.Outputs))
		for i, o := range t.Outputs {
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
		cfg["to"] = t.To
		if t.SendAll {
			cfg["send_all"] = true
		} else {
			cfg["amount_sat"] = t.AmountSat
		}
	}
	if len(t.Inputs) > 0 {
		ins := make([]map[string]interface{}, len(t.Inputs))
		for i, in := range t.Inputs {
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
	if t.FeeRate > 0 {
		cfg["fee_rate_sat_per_vb"] = t.FeeRate
	}
	if t.ChangeAddress != "" {
		cfg["change_address"] = t.ChangeAddress
	}
	if dryRun {
		cfg["dry_run"] = true
	}
	b, _ := json.Marshal(cfg)
	return b
}

// Compile-time assertion that the API satisfies TaskExecutor.
var _ TaskExecutor = (*API)(nil)

// quench unused-import warnings (math + time are pulled by other
// generated forms in this file when build tags shift).
var _ = math.MaxInt64
var _ = time.Second
