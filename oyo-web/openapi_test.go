package main

import (
	"net/http"
	"os"
	"strings"
	"testing"
)

// TestOpenAPISpecCoverage pins the documentation contract: every
// /api/* route registered with the mux must appear in openapi.yaml.
//
// We don't pull a YAML lib in just for this — instead the test scans
// the spec text for `/api/...` path keys, and the registered routes
// for each `mux.HandleFunc("/api/..."` call. Any mux path that's not
// in the spec is a documentation drift.
func TestOpenAPISpecCoverage(t *testing.T) {
	spec, err := os.ReadFile("openapi.yaml")
	if err != nil {
		t.Fatalf("openapi.yaml unreadable: %v", err)
	}
	specText := string(spec)

	// Build a fake mux that just records the registered paths so we
	// see exactly which endpoints the API surface exposes.
	mux := http.NewServeMux()
	a := &API{} // empty handler funcs are fine — we only need the paths
	a.Register(mux)
	// Register attaches the routes via mux.HandleFunc; since http.ServeMux
	// doesn't expose its registered patterns, we mirror Register by
	// scraping api.go directly.
	src, err := os.ReadFile("api.go")
	if err != nil {
		t.Fatalf("api.go unreadable: %v", err)
	}
	registered := scrapeMuxPaths(string(src))
	if len(registered) == 0 {
		t.Fatal("scraped 0 mux paths from api.go — pattern broken?")
	}

	missing := []string{}
	for _, route := range registered {
		// The spec encodes path params as {id}; mux uses trailing /.
		// A registered "/api/queue/" maps to spec keys "/api/queue/{id}"
		// or "/api/queue/{id}/run". Accept either: we just need the
		// prefix to appear somewhere in the spec body.
		if !strings.Contains(specText, route) &&
			!strings.Contains(specText, strings.TrimSuffix(route, "/")) {
			missing = append(missing, route)
		}
	}
	if len(missing) > 0 {
		t.Errorf("openapi.yaml is missing routes:\n  %s", strings.Join(missing, "\n  "))
	}
}

// scrapeMuxPaths extracts every /api/... literal from mux.HandleFunc
// calls in api.go. Quick + textual; would prefer the mux to expose
// its routing table, but http.ServeMux doesn't.
func scrapeMuxPaths(src string) []string {
	var out []string
	for _, line := range strings.Split(src, "\n") {
		l := strings.TrimSpace(line)
		if !strings.Contains(l, `mux.HandleFunc("/api/`) {
			continue
		}
		i := strings.Index(l, `"/api/`)
		if i < 0 {
			continue
		}
		j := strings.Index(l[i+1:], `"`)
		if j < 0 {
			continue
		}
		out = append(out, l[i+1:i+1+j])
	}
	return out
}
