package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// Config holds all oyo-web configuration.
type Config struct {
	Listen      string `json:"listen"`
	RPC         string `json:"rpc"`
	RPCUser     string `json:"rpcuser"`
	RPCPassword string `json:"rpcpassword"`
	WebUser     string `json:"webuser"`
	WebPassword string `json:"webpassword"`
	Prefix      string `json:"prefix"`
	Network     string `json:"network"`
	// DataDir holds the persistent chain mirror (oyoltc-<network>.ldb)
	// and any other on-disk state. When empty defaults to the directory
	// the config file lives in. Use a separate path for test runs so the
	// test server doesn't share mirror state with the usertest server.
	DataDir string `json:"data_dir,omitempty"`
	// TrackMempool enables the liboyoltc mempool sync tailer (per-tick
	// getrawmempool + getrawtransaction loop with seen-not-ours cache).
	// Off by default — appropriate for small/regtest setups; turn on when
	// pending balances are needed in the UI.
	TrackMempool bool `json:"track_mempool"`
	// RollbackWindow is how many recent blocks the engine keeps in memory
	// for fast incremental reorg-undo. A reorg deeper than this still
	// resolves correctly — it just rebuilds the cache from the persistent
	// mirror instead of unwinding block-by-block. 0 → engine default (100);
	// liboyoltc clamps the effective value to 1000.
	RollbackWindow int `json:"rollback_window,omitempty"`
	// NativeBlockParse fetches raw blocks (getblock verbosity 0) and
	// deserializes them natively instead of pulling verbosity=2 JSON — faster
	// on a long genesis walk. Needs a node serving rpcserialversion=2 (raw with
	// witness + MWEB — the OYO/Litecoin default). Default on when absent; set
	// false in the config to force the JSON path. Pointer so "absent" (→ default
	// true) is distinguishable from an explicit false.
	NativeBlockParse *bool `json:"native_block_parse,omitempty"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("reading config %s: %w", path, err)
	}
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("parsing config %s: %w", path, err)
	}
	// Defaults are applied in main() AFTER flag/-chain overrides, so a
	// chain profile can supply its own RPC default when the config (and
	// flags) leave it blank.
	return &cfg, nil
}

// chainProfile maps a user-friendly chain name to liboyoltc's canonical
// network string + the litecoind default RPC URL for that chain. The
// network string feeds address codec selection AND key derivation, so it
// must be exactly "main" / "test" / "regtest".
func chainProfile(chain string) (network, rpcURL string, err error) {
	switch strings.ToLower(chain) {
	case "regtest", "reg":
		return "regtest", "http://127.0.0.1:19443", nil
	case "testnet", "test":
		return "test", "http://127.0.0.1:19332", nil
	case "mainnet", "main":
		return "main", "http://127.0.0.1:9332", nil
	default:
		return "", "", fmt.Errorf("unknown -chain %q (use regtest|testnet|mainnet)", chain)
	}
}

// cliOverrides carries the launch flags that layer over a config file.
// Empty string == "not provided" (so the flag doesn't override).
type cliOverrides struct {
	chain, listen, rpc, rpcUser, rpcPass, dataDir, prefix, webUser, webPass, trackMempool string
}

// resolveConfig layers a config: base (file or empty) → -chain profile →
// explicit flag overrides → defaults. Pure + side-effect free so the
// precedence is unit-testable without spinning a server.
func resolveConfig(base *Config, o cliOverrides) (*Config, error) {
	cfg := *base // copy; never mutate the caller's struct
	if o.chain != "" {
		net, defRPC, err := chainProfile(o.chain)
		if err != nil {
			return nil, err
		}
		cfg.Network = net
		if cfg.RPC == "" {
			cfg.RPC = defRPC
		}
	}
	if o.listen != "" {
		cfg.Listen = o.listen
	}
	if o.rpc != "" {
		cfg.RPC = o.rpc
	}
	if o.rpcUser != "" {
		cfg.RPCUser = o.rpcUser
	}
	if o.rpcPass != "" {
		cfg.RPCPassword = o.rpcPass
	}
	if o.dataDir != "" {
		cfg.DataDir = o.dataDir
	}
	if o.prefix != "" {
		cfg.Prefix = o.prefix
	}
	if o.webUser != "" {
		cfg.WebUser = o.webUser
	}
	if o.webPass != "" {
		cfg.WebPassword = o.webPass
	}
	if o.trackMempool != "" {
		cfg.TrackMempool = o.trackMempool == "true" || o.trackMempool == "1"
	}
	if cfg.Listen == "" {
		cfg.Listen = ":8880"
	}
	if cfg.RPC == "" {
		cfg.RPC = "http://127.0.0.1:9332"
	}
	if cfg.NativeBlockParse == nil {
		def := true
		cfg.NativeBlockParse = &def
	}
	return &cfg, nil
}

func init() {
	log.SetFlags(log.Ldate | log.Ltime)
}

// raiseFileLimit lifts the process open-file soft limit toward the hard limit
// and logs the effective value. The LevelDB UTXO mirror keeps many SST files
// open (max_open_files is set high for read throughput), so a large mainnet
// mirror needs a soft limit well above the common 1024 default or random reads
// surface as "too many open files". The Go runtime already raises soft→hard at
// startup; this is an explicit, logged belt-and-suspenders for long-running
// deployments — pair it with a high LimitNOFILE in the service unit.
func raiseFileLimit() {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		log.Printf("warning: getrlimit(NOFILE) failed: %v", err)
		return
	}
	if lim.Cur < lim.Max {
		want := lim
		want.Cur = lim.Max
		if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &want); err == nil {
			lim.Cur = want.Cur
		} else {
			log.Printf("warning: raising NOFILE soft limit failed: %v", err)
		}
	}
	log.Printf("open-file limit (NOFILE): soft=%d hard=%d", lim.Cur, lim.Max)
}

func main() {
	configPath := flag.String("config", "", "Path to JSON config file (optional when flags / -chain supply the settings)")
	chain := flag.String("chain", "", "Chain profile: regtest | testnet | mainnet (sets network + default RPC port)")
	listen := flag.String("listen", "", "HTTP listen address, e.g. 127.0.0.1:8880 (overrides config)")
	rpcURL := flag.String("rpc", "", "litecoind JSON-RPC URL, e.g. http://127.0.0.1:9332 (overrides config)")
	rpcUser := flag.String("rpcuser", "", "RPC username (overrides config)")
	rpcPass := flag.String("rpcpassword", "", "RPC password (overrides config)")
	dataDir := flag.String("datadir", "", "Data dir for the persistent UTXO mirror (overrides config)")
	prefixFlag := flag.String("prefix", "", "URL path prefix, e.g. /oyo (overrides config)")
	webUser := flag.String("webuser", "", "Basic-auth username (overrides config)")
	webPass := flag.String("webpassword", "", "Basic-auth password (overrides config)")
	trackMempool := flag.String("track-mempool", "", "Mempool tailer: true|false (overrides config)")
	printConfig := flag.Bool("print-config", false, "Print the effective config as JSON and exit (generate a config file)")
	flag.Parse()

	// Layered config: JSON file (if any) → -chain profile → explicit flag
	// overrides → defaults. Lets the server run from a config file, from
	// flags alone (no file), or a mix — addressing the hardcoded-launch gap.
	base := &Config{}
	if *configPath != "" {
		loaded, err := loadConfig(*configPath)
		if err != nil {
			log.Fatal(err)
		}
		base = loaded
	}
	cfg, err := resolveConfig(base, cliOverrides{
		chain: *chain, listen: *listen, rpc: *rpcURL, rpcUser: *rpcUser,
		rpcPass: *rpcPass, dataDir: *dataDir, prefix: *prefixFlag,
		webUser: *webUser, webPass: *webPass, trackMempool: *trackMempool,
	})
	if err != nil {
		log.Fatal(err)
	}

	if *printConfig {
		out, _ := json.MarshalIndent(cfg, "", "  ")
		fmt.Println(string(out))
		return
	}
	if *configPath == "" && *chain == "" && *rpcURL == "" {
		fmt.Fprintf(os.Stderr, "Usage: oyo-web -config <path> | -chain <regtest|testnet|mainnet> [flags]\n\n")
		flag.PrintDefaults()
		os.Exit(1)
	}

	pfx := strings.TrimRight(cfg.Prefix, "/")

	rpc, err := NewRPCClient(cfg.RPC, cfg.RPCUser, cfg.RPCPassword)
	if err != nil {
		log.Fatalf("RPC client init failed: %v", err)
	}

	mux := http.NewServeMux()

	auth := NewAuth(cfg.WebUser, cfg.WebPassword, pfx)
	if auth.Enabled() {
		auth.RegisterRoutes(mux)
		log.Printf("Auth enabled (user: %s)", cfg.WebUser)
	}

	// Mempool watcher
	mempoolWatcher := NewMempoolWatcher(rpc, 2*time.Second)
	mempoolWatcher.Start()

	// API routes at /api/...
	// Persistent chain mirror lives in DataDir. Defaults to the
	// config-file directory when DataDir is empty so single-config
	// setups keep their existing layout. Set DataDir explicitly in
	// the test config so the test server doesn't share the usertest
	// server's oyoltc-*.ldb state.
	workdir := cfg.DataDir
	if workdir == "" {
		workdir = filepath.Dir(*configPath)
	}
	if err := os.MkdirAll(workdir, 0o755); err != nil {
		log.Fatalf("create data dir %s: %v", workdir, err)
	}
	raiseFileLimit()
	api, err := NewAPI(rpc, cfg.Network, workdir, pfx, cfg.TrackMempool, cfg.RollbackWindow, *cfg.NativeBlockParse)
	if err != nil {
		log.Fatalf("API init failed: %v", err)
	}
	defer api.Close()
	api.mempool = mempoolWatcher
	api.Init()
	api.Register(mux)

	// Frontend
	mux.Handle("/", http.FileServer(http.FS(frontendFS)))

	// Wrap with auth
	handler := auth.Middleware(mux)

	srv := &http.Server{
		Addr:    cfg.Listen,
		Handler: handler,
	}

	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
		<-sigCh
		fmt.Println("\nShutting down...")
		srv.Close()
	}()

	if pfx != "" {
		log.Printf("OYO LTC starting on %s (RPC: %s, prefix: %s)", cfg.Listen, cfg.RPC, pfx)
	} else {
		log.Printf("OYO LTC starting on %s (RPC: %s)", cfg.Listen, cfg.RPC)
	}
	if err := srv.ListenAndServe(); err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
