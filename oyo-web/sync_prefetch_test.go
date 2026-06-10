package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestSyncPrefetcherKeepsSlidingAhead(t *testing.T) {
	rpc, closeFn := testPrefetchRPC(t, 128)
	defer closeFn()

	p := newSyncPrefetcher(rpc, 0, 250, 0)
	defer p.Close()

	waitForPrefetchStat(t, p, func(s syncPrefetchStats) bool {
		return s.Scheduled == syncPrefetchAhead && s.Completed == syncPrefetchAhead
	})
	time.Sleep(25 * time.Millisecond)
	if got := p.Snapshot().Scheduled; got != syncPrefetchAhead {
		t.Fatalf("scheduled before consume = %d, want %d", got, syncPrefetchAhead)
	}

	for h := int64(0); h < 10; h++ {
		hashResp, err := p.CallJSON(testRPCBody(t, "getblockhash", []interface{}{h}))
		if err != nil {
			t.Fatalf("getblockhash %d: %v", h, err)
		}
		hash, err := rpcStringResult(hashResp)
		if err != nil {
			t.Fatalf("parse hash %d: %v", h, err)
		}
		if _, err := p.CallJSON(testRPCBody(t, "getblock", []interface{}{hash, int64(0)})); err != nil {
			t.Fatalf("getblock %d: %v", h, err)
		}
	}

	waitForPrefetchStat(t, p, func(s syncPrefetchStats) bool {
		return s.Scheduled == syncPrefetchAhead+10 && s.Completed == syncPrefetchAhead+10
	})
}

func TestSyncPrefetcherReleasesConsumedBlockBytes(t *testing.T) {
	rpc, closeFn := testPrefetchRPC(t, 4096)
	defer closeFn()

	p := newSyncPrefetcher(rpc, 0, 0, 0)
	defer p.Close()

	waitForPrefetchStat(t, p, func(s syncPrefetchStats) bool {
		return s.Completed == 1 && s.BufferedBytes > 0
	})
	hashResp, err := p.CallJSON(testRPCBody(t, "getblockhash", []interface{}{int64(0)}))
	if err != nil {
		t.Fatalf("getblockhash: %v", err)
	}
	hash, err := rpcStringResult(hashResp)
	if err != nil {
		t.Fatalf("parse hash: %v", err)
	}
	if _, err := p.CallJSON(testRPCBody(t, "getblock", []interface{}{hash, int64(0)})); err != nil {
		t.Fatalf("getblock: %v", err)
	}
	if got := p.Snapshot().BufferedBytes; got != 0 {
		t.Fatalf("buffered bytes after consume = %d, want 0", got)
	}
}

func testPrefetchRPC(t *testing.T, blockBytes int) (*RPCClient, func()) {
	t.Helper()
	blockPayload := strings.Repeat("a", blockBytes)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var req syncRPCRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Fatalf("decode request: %v", err)
		}
		var result interface{}
		switch req.Method {
		case "getblockhash":
			h, ok := requestInt64Param(req, 0)
			if !ok {
				t.Fatalf("bad getblockhash params: %q", string(req.Params[0]))
			}
			result = fmt.Sprintf("hash-%06d", h)
		case "getblock":
			hash, ok := requestStringParam(req, 0)
			if !ok {
				t.Fatalf("bad getblock params")
			}
			result = hash + ":" + blockPayload
		default:
			t.Fatalf("unexpected method %q", req.Method)
		}
		_ = json.NewEncoder(w).Encode(RPCResponse{
			JSONRPC: "1.0",
			ID:      "test",
			Result:  mustJSONRaw(t, result),
		})
	}))
	rpc, err := NewRPCClient(srv.URL, "user", "pass")
	if err != nil {
		srv.Close()
		t.Fatalf("new rpc client: %v", err)
	}
	return rpc, srv.Close
}

func testRPCBody(t *testing.T, method string, params []interface{}) []byte {
	t.Helper()
	body, err := makeRPCBody(method, params)
	if err != nil {
		t.Fatalf("make rpc body: %v", err)
	}
	return body
}

func mustJSONRaw(t *testing.T, v interface{}) json.RawMessage {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal result: %v", err)
	}
	return b
}

func waitForPrefetchStat(t *testing.T, p *syncPrefetcher, ok func(syncPrefetchStats) bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if ok(p.Snapshot()) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("prefetch stats did not reach expected state: %+v", p.Snapshot())
}
