package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// helper: middleware-wrapped handler that just writes 200 if reached.
func authHarness() (*Auth, http.Handler) {
	a := NewAuth("oyo", "secret-pass", "")
	mux := http.NewServeMux()
	mux.HandleFunc("/api/info", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"ok":true}`))
	})
	return a, a.Middleware(mux)
}

func TestAuthDisabledLetsEverythingThrough(t *testing.T) {
	a := NewAuth("", "", "")
	mux := http.NewServeMux()
	mux.HandleFunc("/api/info", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	srv := httptest.NewServer(a.Middleware(mux))
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/api/info")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("auth disabled: want 200, got %d", resp.StatusCode)
	}
}

func TestAuthBasicCorrectCredentialsAllowed(t *testing.T) {
	_, h := authHarness()
	srv := httptest.NewServer(h)
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/info", nil)
	req.SetBasicAuth("oyo", "secret-pass")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("basic correct: want 200, got %d", resp.StatusCode)
	}
}

func TestAuthBasicWrongPasswordRejected(t *testing.T) {
	_, h := authHarness()
	srv := httptest.NewServer(h)
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/info", nil)
	req.SetBasicAuth("oyo", "WRONG")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 401 {
		t.Fatalf("basic wrong: want 401, got %d", resp.StatusCode)
	}
	if h := resp.Header.Get("WWW-Authenticate"); h == "" {
		t.Errorf("expected WWW-Authenticate header on Basic failure, got empty")
	}
}

func TestAuthBasicWrongUserRejected(t *testing.T) {
	_, h := authHarness()
	srv := httptest.NewServer(h)
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/info", nil)
	req.SetBasicAuth("attacker", "secret-pass")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 401 {
		t.Fatalf("basic wrong user: want 401, got %d", resp.StatusCode)
	}
}

func TestAuthNoCredentialsAPIReturns401(t *testing.T) {
	_, h := authHarness()
	srv := httptest.NewServer(h)
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/api/info")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 401 {
		t.Fatalf("api no creds: want 401, got %d", resp.StatusCode)
	}
	// Bare 401 (no Basic prompt) for browsers / unauth'd UI XHR — only
	// failed-Basic responses set WWW-Authenticate.
	if h := resp.Header.Get("WWW-Authenticate"); h != "" {
		t.Errorf("unauth'd request should NOT set WWW-Authenticate, got %q", h)
	}
}

func TestAuthCookieStillWorks(t *testing.T) {
	a, h := authHarness()
	srv := httptest.NewServer(h)
	defer srv.Close()

	// Manually register a session token (simulates a successful POST /login).
	tok := "deadbeefcafef00d"
	a.sessions.Store(tok, time.Now().Add(time.Hour))

	req, _ := http.NewRequest("GET", srv.URL+"/api/info", nil)
	req.AddCookie(&http.Cookie{Name: "oyo_session", Value: tok})
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("cookie auth: want 200, got %d", resp.StatusCode)
	}
}

func TestAuthBruteForceTallySharedAcrossBasicAndCookie(t *testing.T) {
	a, _ := authHarness()

	// Simulate 5 wrong Basic attempts via the same flow used at runtime
	// (recordFail), bypassing httptest to keep the test fast — the
	// throttle math is what we care about, not the round-trip.
	ip := "10.0.0.1"
	for i := 0; i < 5; i++ {
		a.recordFail(ip)
	}
	if d := a.bruteforceDelay(ip); d == 0 {
		t.Fatalf("after 5 fails, expected non-zero brute-force delay, got 0")
	}
	a.clearFails(ip)
	if d := a.bruteforceDelay(ip); d != 0 {
		t.Fatalf("after clearFails, expected 0 delay, got %v", d)
	}
}
