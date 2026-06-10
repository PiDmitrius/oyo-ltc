package main

import (
	"strings"
	"testing"
)

// TestInjectServerURL pins the contract that the openapi.yaml served
// to clients has a single dynamic `servers:` entry — so Swagger UI's
// "Try it out" lands at the actual deployment URL even behind a
// reverse-proxy prefix, and the dropdown collapses to one server.
func TestInjectServerURL(t *testing.T) {
	src := []byte(`openapi: 3.0.3
info:
  title: foo
servers:
  - url: http://localhost:8880
    description: prod
  - url: http://localhost:8881
    description: test

# next top-level
security:
  - basicAuth: []

tags:
  - name: wallet
`)
	out := string(injectServerURL(src, "https://example.com/oyo"))

	// Old localhost servers must be gone.
	if strings.Contains(out, "localhost:8880") {
		t.Fatalf("output still contains old server URL:\n%s", out)
	}
	if strings.Contains(out, "localhost:8881") {
		t.Fatalf("output still contains old server URL:\n%s", out)
	}

	// New server URL must be present, exactly once.
	if c := strings.Count(out, "https://example.com/oyo"); c != 1 {
		t.Fatalf("want 1 occurrence of injected URL, got %d:\n%s", c, out)
	}

	// Subsequent top-level keys must still be intact (no spillover).
	for _, want := range []string{"openapi: 3.0.3", "info:", "security:", "tags:"} {
		if !strings.Contains(out, want) {
			t.Errorf("post-injection YAML missing %q", want)
		}
	}

	// The injected line must use the exact form the spec emits.
	if !strings.Contains(out, "servers:\n  - url: https://example.com/oyo\n") {
		t.Errorf("injected `servers:` block is malformed:\n%s", out)
	}
}

func TestInjectServerURLWithoutTrailingPrefix(t *testing.T) {
	// Empty prefix — base is just scheme://host. Same shape, no path
	// suffix on the URL.
	src := []byte("servers:\n  - url: http://x:1\n\nsecurity:\n  - basicAuth: []\n")
	out := string(injectServerURL(src, "http://localhost:8881"))
	if !strings.Contains(out, "  - url: http://localhost:8881\n") {
		t.Fatalf("output missing exact url line:\n%s", out)
	}
	if strings.Contains(out, "x:1") {
		t.Fatalf("old server still present:\n%s", out)
	}
}

func TestInjectServerURLNoBlockReturnsUnchanged(t *testing.T) {
	// Spec without a servers: block — function returns input as-is.
	src := []byte("openapi: 3.0.3\ninfo:\n  title: foo\n")
	out := injectServerURL(src, "http://x")
	if string(out) != string(src) {
		t.Fatalf("expected unchanged, got:\n%s", out)
	}
}
