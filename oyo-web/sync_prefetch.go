package main

import (
	"encoding/json"
	"fmt"
	"sync"
	"time"
)

const (
	syncPrefetchAhead    = 100
	syncPrefetchWorkers  = 8
	syncPrefetchMaxBytes = 256 * 1024 * 1024
)

type syncPrefetcher struct {
	rpc       *RPCClient
	verbosity int64

	mu            sync.Mutex
	byHeight      map[int64]*syncPrefetchEntry
	byHash        map[string]*syncPrefetchEntry
	jobs          chan *syncPrefetchEntry
	stop          chan struct{}
	stopOnce      sync.Once
	closed        bool
	target        int64
	nextHeight    int64
	bufferedBytes int64
	stats         syncPrefetchStats
}

type syncPrefetchEntry struct {
	height    int64
	hash      string
	hashResp  []byte
	blockResp []byte
	err       error
	done      chan struct{}
}

type syncPrefetchStats struct {
	Enabled       bool  `json:"enabled"`
	StartHeight   int64 `json:"start_height,omitempty"`
	EndHeight     int64 `json:"end_height,omitempty"`
	Window        int64 `json:"window"`
	Workers       int64 `json:"workers"`
	MaxBytes      int64 `json:"max_bytes"`
	BufferedBytes int64 `json:"buffered_bytes"`
	Scheduled     int64 `json:"scheduled"`
	Completed     int64 `json:"completed"`
	Errors        int64 `json:"errors"`
	HashHits      int64 `json:"hash_hits"`
	BlockHits     int64 `json:"block_hits"`
	HashMisses    int64 `json:"hash_misses"`
	BlockMisses   int64 `json:"block_misses"`
	WaitTimeouts  int64 `json:"wait_timeouts"`
	TotalWaitMs   int64 `json:"total_wait_ms"`
}

type syncRPCRequest struct {
	Method string            `json:"method"`
	Params []json.RawMessage `json:"params"`
}

func newSyncPrefetcher(rpc *RPCClient, start, target, verbosity int64) *syncPrefetcher {
	if rpc == nil || start < 0 || target < start {
		return nil
	}
	p := &syncPrefetcher{
		rpc:        rpc,
		verbosity:  verbosity,
		byHeight:   make(map[int64]*syncPrefetchEntry, syncPrefetchAhead),
		byHash:     make(map[string]*syncPrefetchEntry, syncPrefetchAhead),
		jobs:       make(chan *syncPrefetchEntry, syncPrefetchAhead),
		stop:       make(chan struct{}),
		target:     target,
		nextHeight: start,
		stats: syncPrefetchStats{
			Enabled:     true,
			StartHeight: start,
			EndHeight:   target,
			Window:      syncPrefetchAhead,
			Workers:     syncPrefetchWorkers,
			MaxBytes:    syncPrefetchMaxBytes,
		},
	}
	for i := 0; i < syncPrefetchWorkers; i++ {
		go func() {
			for {
				select {
				case <-p.stop:
					return
				case e := <-p.jobs:
					select {
					case <-p.stop:
						e.err = fmt.Errorf("prefetch stopped")
						close(e.done)
						return
					default:
					}
					p.fetch(e)
				}
			}
		}()
	}
	p.scheduleMore()
	return p
}

func (p *syncPrefetcher) Close() {
	if p == nil {
		return
	}
	p.stopOnce.Do(func() { close(p.stop) })
	p.mu.Lock()
	p.closed = true
	p.mu.Unlock()
}

func (p *syncPrefetcher) Snapshot() syncPrefetchStats {
	if p == nil {
		return syncPrefetchStats{Enabled: false}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	p.stats.BufferedBytes = p.bufferedBytes
	return p.stats
}

func (p *syncPrefetcher) addStat(fn func(*syncPrefetchStats)) {
	p.mu.Lock()
	defer p.mu.Unlock()
	fn(&p.stats)
}

func (p *syncPrefetcher) scheduleMore() {
	var jobs []*syncPrefetchEntry
	p.mu.Lock()
	for !p.closed && p.nextHeight <= p.target &&
		len(p.byHeight) < syncPrefetchAhead &&
		(p.bufferedBytes < syncPrefetchMaxBytes || len(p.byHeight) == 0) {
		e := &syncPrefetchEntry{height: p.nextHeight, done: make(chan struct{})}
		p.byHeight[e.height] = e
		p.nextHeight++
		p.stats.Scheduled++
		jobs = append(jobs, e)
	}
	p.mu.Unlock()
	for _, e := range jobs {
		select {
		case p.jobs <- e:
		case <-p.stop:
			e.err = fmt.Errorf("prefetch stopped")
			close(e.done)
		}
	}
}

func (p *syncPrefetcher) fetch(e *syncPrefetchEntry) {
	defer close(e.done)
	hashReq, err := makeRPCBody("getblockhash", []interface{}{e.height})
	if err != nil {
		e.err = err
		p.addStat(func(s *syncPrefetchStats) { s.Errors++ })
		return
	}
	hashResp, err := p.rpc.CallJSON(hashReq)
	if err != nil {
		e.err = err
		p.addStat(func(s *syncPrefetchStats) { s.Errors++ })
		return
	}
	hash, err := rpcStringResult(hashResp)
	if err != nil {
		e.err = err
		p.addStat(func(s *syncPrefetchStats) { s.Errors++ })
		return
	}
	e.hash = hash
	e.hashResp = hashResp
	p.mu.Lock()
	p.byHash[hash] = e
	p.mu.Unlock()

	blockReq, err := makeRPCBody("getblock", []interface{}{hash, p.verbosity})
	if err != nil {
		e.err = err
		p.addStat(func(s *syncPrefetchStats) { s.Errors++ })
		return
	}
	blockResp, err := p.rpc.CallJSON(blockReq)
	if err != nil {
		e.err = err
		p.addStat(func(s *syncPrefetchStats) { s.Errors++ })
		return
	}
	e.blockResp = blockResp
	p.addBufferedBytes(int64(len(blockResp)))
	p.addStat(func(s *syncPrefetchStats) { s.Completed++ })
}

func (p *syncPrefetcher) addBufferedBytes(n int64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.bufferedBytes += n
	p.stats.BufferedBytes = p.bufferedBytes
}

func (p *syncPrefetcher) CallJSON(body []byte) ([]byte, error) {
	req, ok := parseSyncRPCRequest(body)
	if ok {
		switch req.Method {
		case "getblockhash":
			if h, ok := requestInt64Param(req, 0); ok {
				if resp, ok := p.waitHeight(h, true); ok {
					p.addStat(func(s *syncPrefetchStats) { s.HashHits++ })
					return resp, nil
				}
				p.addStat(func(s *syncPrefetchStats) { s.HashMisses++ })
			}
		case "getblock":
			if hash, ok := requestStringParam(req, 0); ok {
				if resp, ok := p.waitHash(hash); ok {
					p.addStat(func(s *syncPrefetchStats) { s.BlockHits++ })
					return resp, nil
				}
				p.addStat(func(s *syncPrefetchStats) { s.BlockMisses++ })
			}
		}
	}
	return p.rpc.CallJSON(body)
}

func (p *syncPrefetcher) waitHeight(height int64, hashOnly bool) ([]byte, bool) {
	p.mu.Lock()
	e := p.byHeight[height]
	p.mu.Unlock()
	if e == nil {
		return nil, false
	}
	waited, ok := waitDone(e.done, 2*time.Second)
	p.addStat(func(s *syncPrefetchStats) { s.TotalWaitMs += waited.Milliseconds() })
	if !ok {
		p.addStat(func(s *syncPrefetchStats) { s.WaitTimeouts++ })
		return nil, false
	}
	if e.err != nil {
		return nil, false
	}
	if hashOnly && len(e.hashResp) > 0 {
		return e.hashResp, true
	}
	if len(e.blockResp) > 0 {
		return e.blockResp, true
	}
	return nil, false
}

func (p *syncPrefetcher) waitHash(hash string) ([]byte, bool) {
	p.mu.Lock()
	e := p.byHash[hash]
	p.mu.Unlock()
	if e == nil {
		return nil, false
	}
	waited, ok := waitDone(e.done, 2*time.Second)
	p.addStat(func(s *syncPrefetchStats) { s.TotalWaitMs += waited.Milliseconds() })
	if !ok {
		p.addStat(func(s *syncPrefetchStats) { s.WaitTimeouts++ })
		return nil, false
	}
	if e.err != nil || len(e.blockResp) == 0 {
		return nil, false
	}
	resp := e.blockResp
	p.consume(e)
	return resp, true
}

func (p *syncPrefetcher) consume(e *syncPrefetchEntry) {
	p.mu.Lock()
	if cur := p.byHeight[e.height]; cur == e {
		delete(p.byHeight, e.height)
	}
	if e.hash != "" {
		if cur := p.byHash[e.hash]; cur == e {
			delete(p.byHash, e.hash)
		}
	}
	if n := int64(len(e.blockResp)); n > 0 {
		p.bufferedBytes -= n
		if p.bufferedBytes < 0 {
			p.bufferedBytes = 0
		}
		p.stats.BufferedBytes = p.bufferedBytes
	}
	e.hashResp = nil
	e.blockResp = nil
	p.mu.Unlock()
	p.scheduleMore()
}

func waitDone(done <-chan struct{}, timeout time.Duration) (time.Duration, bool) {
	start := time.Now()
	select {
	case <-done:
		return time.Since(start), true
	case <-time.After(timeout):
		return time.Since(start), false
	}
}

func parseSyncRPCRequest(body []byte) (syncRPCRequest, bool) {
	var req syncRPCRequest
	if err := json.Unmarshal(body, &req); err != nil || req.Method == "" {
		return syncRPCRequest{}, false
	}
	return req, true
}

func requestInt64Param(req syncRPCRequest, idx int) (int64, bool) {
	if len(req.Params) <= idx {
		return 0, false
	}
	var v int64
	if err := json.Unmarshal(req.Params[idx], &v); err != nil {
		return 0, false
	}
	return v, true
}

func requestStringParam(req syncRPCRequest, idx int) (string, bool) {
	if len(req.Params) <= idx {
		return "", false
	}
	var v string
	if err := json.Unmarshal(req.Params[idx], &v); err != nil || v == "" {
		return "", false
	}
	return v, true
}

func makeRPCBody(method string, params []interface{}) ([]byte, error) {
	return json.Marshal(RPCRequest{
		JSONRPC: "1.0",
		ID:      "oyo-prefetch",
		Method:  method,
		Params:  params,
	})
}

func rpcStringResult(body []byte) (string, error) {
	var resp RPCResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", err
	}
	if resp.Error != nil {
		return "", resp.Error
	}
	var s string
	if err := json.Unmarshal(resp.Result, &s); err != nil {
		return "", fmt.Errorf("RPC result is not string: %w", err)
	}
	return s, nil
}
