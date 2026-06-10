package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"
)

// Auth handles login/session management.
type Auth struct {
	user     string
	pass     string
	prefix   string
	sessions sync.Map // token -> expiry
	failsMu  sync.Mutex
	fails    map[string]*failInfo // IP -> fail tracking
}

type failInfo struct {
	count    int
	lastFail time.Time
}

// NewAuth creates auth middleware. If user is empty, auth is disabled.
func NewAuth(user, pass, prefix string) *Auth {
	return &Auth{user: user, pass: pass, prefix: prefix, fails: make(map[string]*failInfo)}
}

// bruteforceDelay returns how long to sleep before responding to a login attempt.
// Exponential backoff: 0, 1s, 2s, 4s, 8s, 16s (cap).
func (a *Auth) bruteforceDelay(ip string) time.Duration {
	a.failsMu.Lock()
	defer a.failsMu.Unlock()

	fi, ok := a.fails[ip]
	if !ok || time.Since(fi.lastFail) > 30*time.Minute {
		return 0
	}
	shifts := fi.count
	if shifts > 4 {
		shifts = 4
	}
	return time.Duration(1<<shifts) * time.Second
}

func (a *Auth) recordFail(ip string) {
	a.failsMu.Lock()
	defer a.failsMu.Unlock()

	fi, ok := a.fails[ip]
	if !ok {
		a.fails[ip] = &failInfo{count: 1, lastFail: time.Now()}
		return
	}
	// Reset if last fail was >30 min ago
	if time.Since(fi.lastFail) > 30*time.Minute {
		fi.count = 1
	} else {
		fi.count++
	}
	fi.lastFail = time.Now()
}

func (a *Auth) clearFails(ip string) {
	a.failsMu.Lock()
	defer a.failsMu.Unlock()
	delete(a.fails, ip)
}

func clientIP(r *http.Request) string {
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		parts := strings.SplitN(xff, ",", 2)
		return strings.TrimSpace(parts[0])
	}
	host := r.RemoteAddr
	if idx := strings.LastIndex(host, ":"); idx != -1 {
		host = host[:idx]
	}
	return host
}

// Enabled returns true if auth is configured.
func (a *Auth) Enabled() bool {
	return a.user != ""
}

// Middleware wraps a handler with auth check. Two paths run in
// parallel:
//
//  1. HTTP Basic — `Authorization: Basic base64(user:pass)`. Stateless,
//     suited to programmatic API clients (curl, Python `requests`,
//     codegen). Uses the same `webuser` / `webpassword` config as
//     cookie login. WWW-Authenticate is only set on a failed Basic
//     attempt so browsers without an Authorization header don't get
//     the native popup — they fall through to the cookie path.
//
//  2. Cookie session — set by POST /login, validated against the
//     in-memory session map. Drives the browser UI.
//
// Brute-force throttling (recordFail / bruteforceDelay) is shared
// across both paths so an attacker can't pivot between them.
func (a *Auth) Middleware(next http.Handler) http.Handler {
	if !a.Enabled() {
		return next
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Login/logout and static assets always accessible (routes are at /)
		if r.URL.Path == "/login" || r.URL.Path == "/logout" || r.URL.Path == "/apple-touch-icon.png" {
			next.ServeHTTP(w, r)
			return
		}

		// Basic Auth path — only kicks in when the client explicitly
		// sends an Authorization header. Header missing → fall through
		// to cookie. Header wrong → 401 + brute-force tally.
		if u, p, ok := r.BasicAuth(); ok {
			ip := clientIP(r)
			if delay := a.bruteforceDelay(ip); delay > 0 {
				time.Sleep(delay)
			}
			userOK := subtle.ConstantTimeCompare([]byte(u), []byte(a.user)) == 1
			passOK := subtle.ConstantTimeCompare([]byte(p), []byte(a.pass)) == 1
			if userOK && passOK {
				a.clearFails(ip)
				next.ServeHTTP(w, r)
				return
			}
			a.recordFail(ip)
			w.Header().Set("WWW-Authenticate", `Basic realm="oyo-web"`)
			http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			return
		}

		// Cookie session path
		cookie, err := r.Cookie("oyo_session")
		if err != nil || !a.validSession(cookie.Value) {
			if isAPIRequest(r) {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
			} else {
				// Redirect using browser-visible base path
				http.Redirect(w, r, a.prefix+"/login", http.StatusFound)
			}
			return
		}

		next.ServeHTTP(w, r)
	})
}

func isAPIRequest(r *http.Request) bool {
	return len(r.URL.Path) >= 4 && r.URL.Path[:4] == "/api"
}

func (a *Auth) validSession(token string) bool {
	if v, ok := a.sessions.Load(token); ok {
		if exp, ok := v.(time.Time); ok && time.Now().Before(exp) {
			return true
		}
		a.sessions.Delete(token)
	}
	return false
}

func (a *Auth) createSession() string {
	b := make([]byte, 32)
	rand.Read(b)
	token := hex.EncodeToString(b)
	a.sessions.Store(token, time.Now().Add(24*time.Hour))
	return token
}

// RegisterRoutes adds login/logout endpoints.
func (a *Auth) RegisterRoutes(mux *http.ServeMux) {
	if !a.Enabled() {
		return
	}

	browserRoot := a.prefix + "/"
	browserLogin := a.prefix + "/login"

	mux.HandleFunc("/login", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			if cookie, err := r.Cookie("oyo_session"); err == nil && a.validSession(cookie.Value) {
				http.Redirect(w, r, browserRoot, http.StatusFound)
				return
			}
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			page := strings.ReplaceAll(loginPage, "{{ACTION}}", browserLogin)
			w.Write([]byte(page))
			return
		}

		if r.Method == "POST" {
			r.ParseForm()
			user := r.FormValue("user")
			pass := r.FormValue("pass")
			ip := clientIP(r)

			// Brute-force protection: delay before processing
			if delay := a.bruteforceDelay(ip); delay > 0 {
				log.Printf("Login throttled for %s (%v)", ip, delay)
				time.Sleep(delay)
			}

			userOK := subtle.ConstantTimeCompare([]byte(user), []byte(a.user)) == 1
			passOK := subtle.ConstantTimeCompare([]byte(pass), []byte(a.pass)) == 1

			if userOK && passOK {
				a.clearFails(ip)
				token := a.createSession()
				http.SetCookie(w, &http.Cookie{
					Name:     "oyo_session",
					Value:    token,
					Path:     browserRoot,
					MaxAge:   86400,
					HttpOnly: true,
					SameSite: http.SameSiteLaxMode,
				})
				log.Printf("Login successful for user %s", user)
				http.Redirect(w, r, browserRoot, http.StatusFound)
				return
			}

			a.recordFail(ip)
			log.Printf("Login failed for %s from %s (attempt %d)", user, ip, a.fails[ip].count)
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.WriteHeader(http.StatusUnauthorized)
			page := strings.ReplaceAll(loginPageError, "{{ACTION}}", browserLogin)
			w.Write([]byte(page))
			return
		}

		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	})

	mux.HandleFunc("/logout", func(w http.ResponseWriter, r *http.Request) {
		if cookie, err := r.Cookie("oyo_session"); err == nil {
			a.sessions.Delete(cookie.Value)
		}
		http.SetCookie(w, &http.Cookie{
			Name:   "oyo_session",
			Value:  "",
			Path:   browserRoot,
			MaxAge: -1,
		})
		http.Redirect(w, r, browserLogin, http.StatusFound)
	})
}

const loginCSS = `
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  font-family: 'SF Mono', 'Cascadia Code', 'Consolas', monospace;
  background: #0a0a0f;
  color: #e0e0e8;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
}
.login-box {
  background: #12121a;
  border: 1px solid #1e1e2e;
  border-radius: 12px;
  padding: 40px;
  width: 360px;
}
.login-box h1 {
  color: #4fc3f7;
  font-size: 24px;
  margin-bottom: 24px;
  text-align: center;
}
.form-group {
  margin-bottom: 16px;
}
.form-group label {
  display: block;
  color: #888;
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  margin-bottom: 6px;
}
input {
  width: 100%;
  background: #0a0a0f;
  border: 1px solid #1e1e2e;
  color: #e0e0e8;
  padding: 10px 14px;
  border-radius: 6px;
  font-family: inherit;
  font-size: 14px;
  outline: none;
}
input:focus { border-color: #4fc3f7; }
button {
  width: 100%;
  background: #4fc3f7;
  color: #000;
  border: none;
  padding: 10px;
  border-radius: 6px;
  font-family: inherit;
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  margin-top: 8px;
}
button:hover { opacity: 0.85; }
.error-msg {
  color: #ef5350;
  font-size: 13px;
  text-align: center;
  margin-bottom: 16px;
}
`

const loginPage = `<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OYO LTC - Login</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><circle cx='16' cy='16' r='15' fill='%230a0a0f' stroke='%234fc3f7' stroke-width='2'/><text x='16' y='22' text-anchor='middle' font-family='monospace' font-size='18' font-weight='bold' fill='%234fc3f7'>O</text></svg>">
<style>` + loginCSS + `</style>
</head><body>
<div class="login-box">
  <h1>OYO LTC</h1>
  <form method="POST" action="{{ACTION}}">
    <div class="form-group">
      <label>Username</label>
      <input type="text" name="user" autocomplete="username" autofocus>
    </div>
    <div class="form-group">
      <label>Password</label>
      <input type="password" name="pass" autocomplete="current-password">
    </div>
    <button type="submit">Login</button>
  </form>
</div>
</body></html>`

const loginPageError = `<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OYO LTC - Login</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><circle cx='16' cy='16' r='15' fill='%230a0a0f' stroke='%234fc3f7' stroke-width='2'/><text x='16' y='22' text-anchor='middle' font-family='monospace' font-size='18' font-weight='bold' fill='%234fc3f7'>O</text></svg>">
<style>` + loginCSS + `</style>
</head><body>
<div class="login-box">
  <h1>OYO LTC</h1>
  <div class="error-msg">Invalid username or password</div>
  <form method="POST" action="{{ACTION}}">
    <div class="form-group">
      <label>Username</label>
      <input type="text" name="user" autocomplete="username" autofocus>
    </div>
    <div class="form-group">
      <label>Password</label>
      <input type="password" name="pass" autocomplete="current-password">
    </div>
    <button type="submit">Login</button>
  </form>
</div>
</body></html>`
