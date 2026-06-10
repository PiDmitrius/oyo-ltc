#ifndef OYOLTC_MWEB_H
#define OYOLTC_MWEB_H

// Internal MWEB helpers used by liboyoltc. Not part of the public C ABI;
// public ABI lives in oyoltc.h. The smoke-test C export is declared there
// (oyo_mweb_self_test).

#include <mw/models/crypto/PublicKey.h>
#include <mw/models/crypto/SecretKey.h>
#include <mw/models/tx/Transaction.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace oyoltc {

// Forward-decl of the chain-level MwebAddress so mweb.h can refer to
// `MwebAddress*` in the rewind API. Full definition lives in oyoltc.cpp's
// detail namespace; mweb.cpp only carries the pointer through, never
// dereferences it.
namespace detail { struct MwebAddress; }

// Hash for std::array<uint8_t, N> map keys (commitment 33 bytes, output_id
// / shared_secret / scan_secret 32 bytes, B_i 33 bytes). XOR-fold of 8-byte
// words — pubkeys/commitments are already cryptographically random, so a
// trivial fold is sufficient.
template <size_t N>
struct ArrayHash {
    size_t operator()(const std::array<uint8_t, N>& a) const noexcept {
        size_t h = 0;
        for (size_t i = 0; i + sizeof(size_t) <= N; i += sizeof(size_t)) {
            size_t w; std::memcpy(&w, a.data() + i, sizeof(size_t));
            h ^= w;
        }
        // Tail bytes (N % sizeof(size_t) > 0): fold a final partial word.
        if (N % sizeof(size_t) != 0) {
            size_t w = 0;
            std::memcpy(&w, a.data() + N - (N % sizeof(size_t)), N % sizeof(size_t));
            h ^= w;
        }
        return h;
    }
};
using ArrayHash32 = ArrayHash<32>;
using ArrayHash33 = ArrayHash<33>;

namespace mweb {

// 32-byte master seed → master scan/spend secrets via two domain-separated
// SHA-256 derivations. Deterministic; no compatibility guarantees with
// litecoin-core BIP32 derivation (m/0'/100'/…) — that's a separate seed
// type, not what we use for "oyo_mweb_v1".
SecretKey DeriveScanSecret(const std::array<uint8_t, 32>& seed);
SecretKey DeriveSpendSecret(const std::array<uint8_t, 32>& seed);

// Per-address subderivation: spend_key_i = spend_secret + H("OYO-MWEB-IDX",
// scan_secret, i). B_i = PublicKey::From(spend_key_i). A_i = B_i * scan_secret.
SecretKey DeriveSubSpendKey(const SecretKey& scan_secret,
                            const SecretKey& spend_secret,
                            uint32_t index);

struct StealthPair {
    PublicKey scan;   // A_i
    PublicKey spend;  // B_i
};

StealthPair DeriveStealthAddress(const SecretKey& scan_secret,
                                 const SecretKey& spend_secret,
                                 uint32_t index);

// Map key type for spend_pubkey_index (compressed B_i, 33 bytes).
using SpendPubkeyMap = std::unordered_map<
    std::array<uint8_t, 33>,
    detail::MwebAddress*,
    ArrayHash33>;

// Result of trying to rewind a single MWEB output against one keychain.
// `matched=false` means the output is not ours (view-tag mismatch, foreign
// B_i, or commitment verification failed). On match, `matched_address`
// points at the chain-level MwebAddress that owns the output, `amount`
// is decoded, and `shared_secret` is the 32-byte `t` value
// (Hashed(DERIVE, Ke*scan)) needed to re-construct the spend-side
// mw::Coin (blind, spend_key) at send time without re-fetching the
// original Output.
struct RewindResult {
    bool                    matched          = false;
    detail::MwebAddress*    matched_address  = nullptr;
    uint64_t                amount           = 0;
    std::array<uint8_t, 32> shared_secret{};
};

// Pure receive-side rewinder. Operates on raw bytes from getblock
// verbosity=2 (.mweb.outputs[]). spend_pubkey_index is a precomputed
// {compressed B_i bytes → MwebAddress*} map maintained by the wallet on
// every new-address allocation.
//
// Implements the math from libmw `mw::Keychain::RewindOutput` minus the
// LegacyScriptPubKeyMan dependency: identity is established via direct
// lookup in spend_pubkey_index instead of `m_spk_man.GetMetadata`.
RewindResult TryRewindOutput(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const SpendPubkeyMap&          spend_pubkey_index,
    const std::array<uint8_t, 33>& commit_bytes,
    const std::array<uint8_t, 33>& receiver_pubkey_bytes,
    const std::vector<uint8_t>&    message_bytes);

// Same as TryRewindOutput but takes a single byte blob — a serialized
// `mw::Output` (e.g. decoded from a mempool tx's MWEB extension).
// Deserializes the Output, then dispatches to TryRewindOutput. On success out_commit and
// out_output_id are filled with the matched output's identifiers for
// chain.mweb_utxo_index registration.
RewindResult TryRewindOutputBlob(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const SpendPubkeyMap&          spend_pubkey_index,
    const std::vector<uint8_t>&    output_blob,
    std::array<uint8_t, 33>*       out_commit,
    std::array<uint8_t, 32>*       out_output_id);

// Sender-side coin descriptor: minimum fields the wallet needs to keep
// per UTXO (besides commit/output_id/amount) so that BuildMwebSendTx can
// reconstruct the full mw::Coin lazily without re-fetching the Output.
struct CoinSpendInfo {
    uint32_t                address_index = 0;
    uint64_t                amount_sat    = 0;
    std::array<uint8_t, 32> output_id{};
    std::array<uint8_t, 32> shared_secret{};  // `t` from RewindOutput
};

// One recipient on the send side. Address must decode to a StealthAddress
// — caller is responsible for validating the bech32 string before passing
// it in.
struct SendRecipient {
    std::string mweb_address;
    uint64_t    amount_sat = 0;
};

struct BuildMwebSendResult {
    std::string tx_hex;          // serialized CMutableTransaction with mweb extension
    uint64_t    actual_fee_sat;  // canonical = mweb_weight * 100
    uint64_t    change_sat;      // adjusted change (caller fee_sat - actual_fee_sat is folded in)
    uint64_t    weight;          // mweb tx weight (informational)
};

// Builds a fully-signed mw::Transaction for an MWEB→MWEB transfer or an
// MWEB→bech32 peg-out. Inputs are taken from `coins`; recipient amounts
// go to `recipients` (stealth → MWEB recipient, bech32/legacy → peg-out
// kernel); any change goes to `change_address` (caller pre-derives —
// convention is the wallet's own MWEB index 0). Pass `change_amount_sat
// == 0` to omit the change recipient (drains all inputs to recipient,
// useful for Max/send-all flows).
//
// Fee handling — node enforcement:
//   pure MWEB:    fee == mweb_weight * 100
//   peg-out tx:   fee >= mweb_weight * 100 + canonical_vsize * 1 (min relay)
//
// `fee_rate_sat_per_vb` scales the canonical-vsize component for peg-out;
// pass 1 for min-relay, higher for fast confirmation. Pure MWEB ignores
// the rate (canonical_vsize == 0).
//
// `caller_fee_budget_sat` is treated as a ceiling. Helper builds once to
// learn the weight + canonical_vsize, then rebuilds with the exact
// canonical fee, folding the leftover into change (or recipient if
// change_amount==0). Throws if budget can't cover the canonical fee.
BuildMwebSendResult BuildMwebSendTx(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const std::array<uint8_t, 32>& spend_secret_bytes,
    const std::vector<CoinSpendInfo>& coins,
    const std::vector<SendRecipient>& recipients,
    const std::string& change_address,
    uint64_t change_amount_sat,
    uint64_t caller_fee_budget_sat,
    uint64_t fee_rate_sat_per_vb,
    uint32_t nLockTime);

// Result of building the MWEB half of a regular→MWEB peg-in. The caller
// (oyoltc.cpp) embeds `mweb_tx` in a CMutableTransaction and emits a
// canonical peg-in vout `(pegin_amount_sat, GetScriptForPegin(kernel_id))`.
// No canonical change vout is needed when the wallet routes change through
// `change_stealth_addr` on the MWEB side (privacy parity with node default).
struct BuildPegInResult {
    mw::Transaction::CPtr   mweb_tx;
    std::array<uint8_t, 32> kernel_id;
    uint64_t                pegin_amount_sat;     // recipient + change + mweb_fee
    uint64_t                actual_mweb_fee_sat;  // == mweb_weight * 100
    uint64_t                mweb_weight;
};

// Builds the MWEB-side of a peg-in. If `change_amount_sat == 0` the
// resulting tx has a single stealth recipient (mweb_weight = 21,
// mweb_fee = 2100); otherwise an own-side stealth change recipient is
// added (mweb_weight = 39, mweb_fee = 3900). All amounts go *into* the
// MWEB through the kernel's pegin field; no MWEB inputs are spent.
//
// caller_mweb_fee_budget_sat is treated as a ceiling on the mweb-side fee.
// The helper auto-rounds the actual fee to mweb_weight * 100 (the node's
// floor); throws std::runtime_error if the budget can't cover that.
BuildPegInResult BuildPegInMwebPart(
    const std::string& recipient_stealth_addr,
    uint64_t           recipient_amount_sat,
    const std::string& change_stealth_addr,   // ignored when change_amount==0
    uint64_t           change_amount_sat,
    uint64_t           caller_mweb_fee_budget_sat);

}  // namespace mweb
}  // namespace oyoltc

#endif
