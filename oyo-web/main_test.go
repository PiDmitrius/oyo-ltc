package main

import "testing"

func TestChainProfile(t *testing.T) {
	cases := []struct {
		in, net, rpc string
		ok           bool
	}{
		{"regtest", "regtest", "http://127.0.0.1:19443", true},
		{"reg", "regtest", "http://127.0.0.1:19443", true},
		{"testnet", "test", "http://127.0.0.1:19332", true},
		{"mainnet", "main", "http://127.0.0.1:9332", true},
		{"MAIN", "main", "http://127.0.0.1:9332", true},
		{"bogus", "", "", false},
	}
	for _, c := range cases {
		net, rpc, err := chainProfile(c.in)
		if !c.ok {
			if err == nil {
				t.Fatalf("%q: expected error", c.in)
			}
			continue
		}
		if err != nil {
			t.Fatalf("%q: unexpected error %v", c.in, err)
		}
		if net != c.net || rpc != c.rpc {
			t.Fatalf("%q: got (%s,%s) want (%s,%s)", c.in, net, rpc, c.net, c.rpc)
		}
	}
}

func TestResolveConfig(t *testing.T) {
	// -chain on an empty base sets network + default RPC + listen default.
	cfg, err := resolveConfig(&Config{}, cliOverrides{chain: "regtest"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Network != "regtest" || cfg.RPC != "http://127.0.0.1:19443" || cfg.Listen != ":8880" {
		t.Fatalf("regtest profile wrong: %+v", cfg)
	}

	// -chain must NOT clobber an RPC already set in the base config.
	cfg, _ = resolveConfig(&Config{RPC: "http://node:9332"}, cliOverrides{chain: "mainnet"})
	if cfg.Network != "main" || cfg.RPC != "http://node:9332" {
		t.Fatalf("chain clobbered explicit RPC: %+v", cfg)
	}

	// Explicit flags win over both the base config and the -chain default.
	base := &Config{Listen: ":1", RPC: "http://a", Network: "regtest"}
	cfg, _ = resolveConfig(base, cliOverrides{
		chain: "mainnet", listen: "127.0.0.1:9", rpc: "http://b",
		rpcUser: "u", trackMempool: "true",
	})
	if cfg.Listen != "127.0.0.1:9" || cfg.RPC != "http://b" || cfg.RPCUser != "u" ||
		cfg.Network != "main" || !cfg.TrackMempool {
		t.Fatalf("flag overrides wrong: %+v", cfg)
	}
	// resolveConfig must copy — never mutate the caller's base.
	if base.Listen != ":1" || base.RPC != "http://a" {
		t.Fatalf("resolveConfig mutated base: %+v", base)
	}

	// track-mempool=false stays false; an unknown chain errors out.
	cfg, _ = resolveConfig(&Config{}, cliOverrides{chain: "regtest", trackMempool: "false"})
	if cfg.TrackMempool {
		t.Fatal("track-mempool=false should resolve to false")
	}
	if _, err := resolveConfig(&Config{}, cliOverrides{chain: "nope"}); err == nil {
		t.Fatal("unknown chain should error")
	}
}
