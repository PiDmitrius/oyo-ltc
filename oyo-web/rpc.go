package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"
)

// RPCClient handles JSON-RPC communication with litecoind.
type RPCClient struct {
	url    string
	user   string
	pass   string
	client *http.Client
	stats  RPCStats
}

type RPCStats struct {
	mu      sync.Mutex
	methods map[string]*RPCMethodStats
}

type RPCMethodStats struct {
	Count         int64 `json:"count"`
	Errors        int64 `json:"errors"`
	ResponseBytes int64 `json:"response_bytes"`
	TotalMs       int64 `json:"total_ms"`
	MaxMs         int64 `json:"max_ms"`
}

func (s *RPCStats) add(method string, elapsed time.Duration, responseBytes int, failed bool) {
	if method == "" {
		method = "unknown"
	}
	ms := elapsed.Milliseconds()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.methods == nil {
		s.methods = make(map[string]*RPCMethodStats)
	}
	st := s.methods[method]
	if st == nil {
		st = &RPCMethodStats{}
		s.methods[method] = st
	}
	st.Count++
	if failed {
		st.Errors++
	}
	st.ResponseBytes += int64(responseBytes)
	st.TotalMs += ms
	if ms > st.MaxMs {
		st.MaxMs = ms
	}
}

func (s *RPCStats) snapshot() map[string]map[string]int64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string]map[string]int64, len(s.methods))
	for method, st := range s.methods {
		avg := int64(0)
		if st.Count > 0 {
			avg = st.TotalMs / st.Count
		}
		out[method] = map[string]int64{
			"count":          st.Count,
			"errors":         st.Errors,
			"response_bytes": st.ResponseBytes,
			"total_ms":       st.TotalMs,
			"avg_ms":         avg,
			"max_ms":         st.MaxMs,
		}
	}
	return out
}

func rpcMethodFromBody(body []byte) string {
	var req struct {
		Method string `json:"method"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		return "unknown"
	}
	if req.Method == "" {
		return "unknown"
	}
	return req.Method
}

// RPCRequest is a JSON-RPC 1.0 request.
type RPCRequest struct {
	JSONRPC string        `json:"jsonrpc"`
	ID      interface{}   `json:"id"`
	Method  string        `json:"method"`
	Params  []interface{} `json:"params,omitempty"`
}

// RPCResponse is a JSON-RPC 1.0 response.
type RPCResponse struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      interface{}     `json:"id"`
	Result  json.RawMessage `json:"result,omitempty"`
	Error   *RPCError       `json:"error,omitempty"`
}

// RPCError is a JSON-RPC error.
type RPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *RPCError) Error() string {
	return fmt.Sprintf("RPC error %d: %s", e.Code, e.Message)
}

// NewRPCClient creates a new RPC client.
func NewRPCClient(url, user, pass string) (*RPCClient, error) {
	if user == "" {
		return nil, fmt.Errorf("rpcuser is required")
	}
	c := &RPCClient{
		url:  url,
		user: user,
		pass: pass,
		client: &http.Client{
			Timeout: 120 * time.Second,
		},
	}
	return c, nil
}

// getAuth returns the username and password.
func (c *RPCClient) getAuth() (string, string, error) {
	return c.user, c.pass, nil
}

// Call makes a JSON-RPC call.
func (c *RPCClient) Call(method string, params ...interface{}) (*RPCResponse, error) {
	start := time.Now()
	responseBytes := 0
	failed := true
	defer func() {
		c.stats.add(method, time.Since(start), responseBytes, failed)
	}()

	req := RPCRequest{
		JSONRPC: "1.0",
		ID:      "oyo",
		Method:  method,
		Params:  params,
	}

	body, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}

	httpReq, err := http.NewRequest("POST", c.url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Content-Type", "application/json")

	user, pass, err := c.getAuth()
	if err != nil {
		return nil, err
	}
	httpReq.SetBasicAuth(user, pass)

	resp, err := c.client.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	responseBytes = len(respBody)

	var rpcResp RPCResponse
	if err := json.Unmarshal(respBody, &rpcResp); err != nil {
		return nil, fmt.Errorf("invalid RPC response: %s", string(respBody[:min(len(respBody), 200)]))
	}

	if rpcResp.Error != nil {
		return nil, rpcResp.Error
	}

	failed = false
	return &rpcResp, nil
}

// CallJSON posts a fully-formed JSON-RPC request body and returns the raw
// response body. Used by liboyoltc op pumps that emit ready-made requests.
func (c *RPCClient) CallJSON(body []byte) ([]byte, error) {
	method := rpcMethodFromBody(body)
	start := time.Now()
	responseBytes := 0
	failed := true
	defer func() {
		c.stats.add(method, time.Since(start), responseBytes, failed)
	}()

	httpReq, err := http.NewRequest("POST", c.url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Content-Type", "application/json")
	user, pass, err := c.getAuth()
	if err != nil {
		return nil, err
	}
	httpReq.SetBasicAuth(user, pass)
	resp, err := c.client.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	out, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	responseBytes = len(out)
	failed = resp.StatusCode < 200 || resp.StatusCode >= 300
	return out, nil
}

func (c *RPCClient) StatsSnapshot() map[string]map[string]int64 {
	if c == nil {
		return nil
	}
	return c.stats.snapshot()
}

// CallRaw makes a raw JSON-RPC call, returning the raw JSON result.
func (c *RPCClient) CallRaw(method string, params []interface{}) (json.RawMessage, error) {
	resp, err := c.Call(method, params...)
	if err != nil {
		return nil, err
	}
	return resp.Result, nil
}

// CallWallet makes a JSON-RPC call to a specific wallet endpoint (/wallet/<name>).
func (c *RPCClient) CallWallet(wallet, method string, params []interface{}) (json.RawMessage, error) {
	// Build wallet-specific URL
	walletURL := strings.TrimRight(c.url, "/") + "/wallet/" + wallet

	req := RPCRequest{
		JSONRPC: "1.0",
		ID:      "oyo",
		Method:  method,
		Params:  params,
	}

	body, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}

	httpReq, err := http.NewRequest("POST", walletURL, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Content-Type", "application/json")

	user, pass, err := c.getAuth()
	if err != nil {
		return nil, err
	}
	httpReq.SetBasicAuth(user, pass)

	resp, err := c.client.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}

	var rpcResp RPCResponse
	if err := json.Unmarshal(respBody, &rpcResp); err != nil {
		return nil, fmt.Errorf("invalid RPC response: %s", string(respBody[:min(len(respBody), 200)]))
	}

	if rpcResp.Error != nil {
		return nil, rpcResp.Error
	}

	return rpcResp.Result, nil
}
