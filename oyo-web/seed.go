package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/binary"
	"math/big"
)

const (
	pbkdf2Salt       = "litecoin-oyo"
	pbkdf2Iterations = 696969
	pbkdf2KeyLen     = 32

	wifPrefixMainnet = 0xB0 // Litecoin mainnet SECRET_KEY (176); NOT Bitcoin's 0x80
	wifPrefixTestnet = 0xEF // Litecoin testnet + regtest SECRET_KEY (239)
)

// SeedToWIF derives a deterministic WIF private key from an arbitrary string.
// PBKDF2-HMAC-SHA512 with salt "litecoin-oyo", 696969 iterations.
func SeedToWIF(seed string, testnet bool) string {
	key := pbkdf2([]byte(seed), []byte(pbkdf2Salt), pbkdf2Iterations, pbkdf2KeyLen)
	prefix := byte(wifPrefixMainnet)
	if testnet {
		prefix = wifPrefixTestnet
	}
	return encodeWIF(key, prefix)
}

// encodeWIF wraps a 32-byte private key into WIF format (compressed).
func encodeWIF(key []byte, prefix byte) string {
	// prefix(1) + key(32) + compress_flag(1)
	payload := make([]byte, 34)
	payload[0] = prefix
	copy(payload[1:33], key)
	payload[33] = 0x01 // compressed pubkey

	// checksum = first 4 bytes of double SHA-256
	h1 := sha256.Sum256(payload)
	h2 := sha256.Sum256(h1[:])
	return base58Encode(append(payload, h2[:4]...))
}

// base58Encode encodes bytes to base58 (Bitcoin alphabet).
func base58Encode(data []byte) string {
	const alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

	x := new(big.Int).SetBytes(data)
	base := big.NewInt(58)
	zero := big.NewInt(0)
	mod := new(big.Int)

	var result []byte
	for x.Cmp(zero) > 0 {
		x.DivMod(x, base, mod)
		result = append(result, alphabet[mod.Int64()])
	}

	// leading zeros
	for _, b := range data {
		if b != 0 {
			break
		}
		result = append(result, alphabet[0])
	}

	// reverse
	for i, j := 0, len(result)-1; i < j; i, j = i+1, j-1 {
		result[i], result[j] = result[j], result[i]
	}
	return string(result)
}

// pbkdf2 implements PBKDF2-HMAC-SHA512 (RFC 8018).
func pbkdf2(password, salt []byte, iterations, keyLen int) []byte {
	numBlocks := (keyLen + sha512.Size - 1) / sha512.Size
	dk := make([]byte, 0, numBlocks*sha512.Size)

	for block := 1; block <= numBlocks; block++ {
		dk = append(dk, pbkdf2Block(password, salt, iterations, block)...)
	}
	return dk[:keyLen]
}

func pbkdf2Block(password, salt []byte, iterations, block int) []byte {
	mac := hmac.New(sha512.New, password)

	// U1 = HMAC(password, salt || INT_32_BE(block))
	saltBlock := make([]byte, len(salt)+4)
	copy(saltBlock, salt)
	binary.BigEndian.PutUint32(saltBlock[len(salt):], uint32(block))

	mac.Write(saltBlock)
	u := mac.Sum(nil)

	result := make([]byte, len(u))
	copy(result, u)

	// U2..Uc
	for i := 1; i < iterations; i++ {
		mac.Reset()
		mac.Write(u)
		u = mac.Sum(u[:0])
		for j := range result {
			result[j] ^= u[j]
		}
	}
	return result
}
