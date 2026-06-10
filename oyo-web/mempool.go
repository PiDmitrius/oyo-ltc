package main

import (
	"encoding/json"
	"sync"
	"time"
)

// MempoolWatcher tracks pending transactions and maintains per-address pending balances.
type MempoolWatcher struct {
	rpc *RPCClient
	mu  sync.RWMutex

	// txid -> decoded tx info
	txStore map[string]*mempoolTx

	// address -> pending delta (positive = incoming, negative = outgoing)
	pendingMap map[string]float64

	pollInterval time.Duration
	stopCh       chan struct{}
}

type mempoolTx struct {
	TxID    string
	Inputs  []mempoolIO // addresses spent from
	Outputs []mempoolIO // addresses received to
}

type mempoolIO struct {
	Address string
	Amount  float64
}

// NewMempoolWatcher creates a new watcher.
func NewMempoolWatcher(rpc *RPCClient, pollInterval time.Duration) *MempoolWatcher {
	return &MempoolWatcher{
		rpc:          rpc,
		txStore:      make(map[string]*mempoolTx),
		pendingMap:   make(map[string]float64),
		pollInterval: pollInterval,
		stopCh:       make(chan struct{}),
	}
}

// Start begins polling the mempool.
func (mw *MempoolWatcher) Start() {
	go mw.loop()
}

// Stop stops the polling loop.
func (mw *MempoolWatcher) Stop() {
	close(mw.stopCh)
}

// AddrPending holds separated inputs/outputs for an address.
type AddrPending struct {
	Inputs  float64 // total being spent FROM this address (positive value)
	Outputs float64 // total being received AT this address (positive value)
}

// Delta returns net pending (outputs - inputs).
func (ap AddrPending) Delta() float64 {
	return ap.Outputs - ap.Inputs
}

// GetPending returns the pending balance delta for an address.
func (mw *MempoolWatcher) GetPending(address string) float64 {
	mw.mu.RLock()
	defer mw.mu.RUnlock()
	return mw.pendingMap[address]
}

// GetAllPending returns a copy of the full pending map (net deltas).
func (mw *MempoolWatcher) GetAllPending() map[string]float64 {
	mw.mu.RLock()
	defer mw.mu.RUnlock()
	result := make(map[string]float64, len(mw.pendingMap))
	for k, v := range mw.pendingMap {
		result[k] = v
	}
	return result
}

// GetAddrPending returns separated inputs/outputs per address.
func (mw *MempoolWatcher) GetAddrPending() map[string]AddrPending {
	mw.mu.RLock()
	defer mw.mu.RUnlock()
	result := make(map[string]AddrPending)
	for _, tx := range mw.txStore {
		for _, in := range tx.Inputs {
			ap := result[in.Address]
			ap.Inputs += in.Amount
			result[in.Address] = ap
		}
		for _, out := range tx.Outputs {
			ap := result[out.Address]
			ap.Outputs += out.Amount
			result[out.Address] = ap
		}
	}
	return result
}

func (mw *MempoolWatcher) loop() {
	// Initial poll immediately
	mw.poll()
	ticker := time.NewTicker(mw.pollInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			mw.poll()
		case <-mw.stopCh:
			return
		}
	}
}

func (mw *MempoolWatcher) poll() {
	// Get current mempool txids
	raw, err := mw.rpc.CallRaw("getrawmempool", nil)
	if err != nil {
		return
	}
	var txids []string
	if err := json.Unmarshal(raw, &txids); err != nil {
		return
	}

	currentSet := make(map[string]bool, len(txids))
	for _, txid := range txids {
		currentSet[txid] = true
	}

	mw.mu.Lock()
	defer mw.mu.Unlock()

	// Remove txids no longer in mempool
	for txid, tx := range mw.txStore {
		if !currentSet[txid] {
			mw.removeTx(tx)
			delete(mw.txStore, txid)
		}
	}

	// Add new txids
	for _, txid := range txids {
		if _, exists := mw.txStore[txid]; !exists {
			tx := mw.fetchTx(txid)
			if tx != nil {
				mw.txStore[txid] = tx
				mw.addTx(tx)
			}
		}
	}
}

func (mw *MempoolWatcher) fetchTx(txid string) *mempoolTx {
	raw, err := mw.rpc.CallRaw("getrawtransaction", []interface{}{txid, true})
	if err != nil {
		return nil
	}

	var decoded struct {
		TxID string `json:"txid"`
		Vin  []struct {
			TxID string `json:"txid"`
			Vout int    `json:"vout"`
		} `json:"vin"`
		Vout []struct {
			Value        float64 `json:"value"`
			N            int     `json:"n"`
			ScriptPubKey struct {
				Addresses []string `json:"addresses"`
				Address   string   `json:"address"`
			} `json:"scriptPubKey"`
		} `json:"vout"`
	}
	if err := json.Unmarshal(raw, &decoded); err != nil {
		return nil
	}

	tx := &mempoolTx{TxID: decoded.TxID}

	// Outputs: who receives
	for _, vout := range decoded.Vout {
		addr := vout.ScriptPubKey.Address
		if addr == "" && len(vout.ScriptPubKey.Addresses) > 0 {
			addr = vout.ScriptPubKey.Addresses[0]
		}
		if addr != "" && vout.Value > 0 {
			tx.Outputs = append(tx.Outputs, mempoolIO{Address: addr, Amount: vout.Value})
		}
	}

	// Inputs: who spends (need to look up previous tx outputs)
	for _, vin := range decoded.Vin {
		if vin.TxID == "" {
			continue // coinbase
		}
		prevRaw, err := mw.rpc.CallRaw("getrawtransaction", []interface{}{vin.TxID, true})
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
		if json.Unmarshal(prevRaw, &prevTx) != nil {
			continue
		}
		if vin.Vout < len(prevTx.Vout) {
			prev := prevTx.Vout[vin.Vout]
			addr := prev.ScriptPubKey.Address
			if addr == "" && len(prev.ScriptPubKey.Addresses) > 0 {
				addr = prev.ScriptPubKey.Addresses[0]
			}
			if addr != "" {
				tx.Inputs = append(tx.Inputs, mempoolIO{Address: addr, Amount: prev.Value})
			}
		}
	}

	return tx
}

// addTx applies a tx's effect to pendingMap (called with lock held).
func (mw *MempoolWatcher) addTx(tx *mempoolTx) {
	for _, out := range tx.Outputs {
		mw.pendingMap[out.Address] += out.Amount
	}
	for _, in := range tx.Inputs {
		mw.pendingMap[in.Address] -= in.Amount
	}
	mw.cleanZeros()
}

// removeTx reverses a tx's effect on pendingMap (called with lock held).
func (mw *MempoolWatcher) removeTx(tx *mempoolTx) {
	for _, out := range tx.Outputs {
		mw.pendingMap[out.Address] -= out.Amount
	}
	for _, in := range tx.Inputs {
		mw.pendingMap[in.Address] += in.Amount
	}
	mw.cleanZeros()
}

func (mw *MempoolWatcher) cleanZeros() {
	for addr, val := range mw.pendingMap {
		if val > -0.000000005 && val < 0.000000005 {
			delete(mw.pendingMap, addr)
		}
	}
}

// TxCount returns the number of tracked mempool transactions.
func (mw *MempoolWatcher) TxCount() int {
	mw.mu.RLock()
	defer mw.mu.RUnlock()
	return len(mw.txStore)
}

// Refresh forces an immediate mempool poll.
func (mw *MempoolWatcher) Refresh() {
	mw.poll()
}
