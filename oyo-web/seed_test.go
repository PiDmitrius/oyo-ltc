package main

import (
	"encoding/hex"
	"testing"
)

func TestPBKDF2Reference(t *testing.T) {
	// PBKDF2-HMAC-SHA512("password", "salt", 1, 64) - standard reference
	key := pbkdf2([]byte("password"), []byte("salt"), 1, 64)
	got := hex.EncodeToString(key)
	want := "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce"
	if got != want {
		t.Fatalf("PBKDF2 mismatch:\n  got:  %s\n  want: %s", got, want)
	}
}

func TestSeedToWIF(t *testing.T) {
	// PBKDF2-HMAC-SHA512("hello world", "litecoin-oyo", 100, 32) -> WIF
	key := pbkdf2([]byte("hello world"), []byte("litecoin-oyo"), 100, 32)
	got := hex.EncodeToString(key)
	want := "89db45b9b6fd62b7fea29259a0459311a74d8182d7b54e06968a170a57e305b7"
	if got != want {
		t.Fatalf("PBKDF2 mismatch:\n  got:  %s\n  want: %s", got, want)
	}

	wif := encodeWIF(key, wifPrefixTestnet)
	wantWIF := "cSCgCkwwwKtubz527BoKeJ3JGPrvQBFV353YLt4u4HKKBLJuix8b"
	if wif != wantWIF {
		t.Fatalf("WIF mismatch:\n  got:  %s\n  want: %s", wif, wantWIF)
	}

	// Network byte must match the chain: LTC mainnet SECRET_KEY 0xB0 → compressed
	// WIF starts with 'T'; testnet/regtest 0xEF → 'c'. (Bitcoin's 0x80 → 'K'/'L',
	// which sethdseed rejects on a Litecoin mainnet node — the node-wallet bug.)
	if m := SeedToWIF("hello world", false); len(m) == 0 || m[0] != 'T' {
		t.Fatalf("mainnet WIF must start with 'T' (LTC 0xB0), got %q", m)
	}
	if tw := SeedToWIF("hello world", true); len(tw) == 0 || tw[0] != 'c' {
		t.Fatalf("testnet WIF must start with 'c' (0xEF), got %q", tw)
	}
}

func TestBase58Encode(t *testing.T) {
	vectors := []struct {
		hex  string
		want string
	}{
		{"", ""},
		{"00", "1"},
		{"0000", "11"},
		{"01", "2"},
		{"0001", "12"},
		{"48656c6c6f20576f726c64", "JxF12TrwUP45BMd"},
		{"0000000000000000000000000000000000000000", "11111111111111111111"},
		{"003c176e659bea0f29a3e9bf7880c112b1b31b4dc826268187", "16UjcYNBG9GTK4uq2f7yYEbuifqCzoLMGS"},
	}
	for _, v := range vectors {
		data, _ := hex.DecodeString(v.hex)
		got := base58Encode(data)
		if got != v.want {
			t.Fatalf("base58(%s): got %q, want %q", v.hex, got, v.want)
		}
	}
}
