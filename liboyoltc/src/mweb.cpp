#include "mweb.h"
#include "oyoltc.h"

#include <crypto/sha256.h>
#include <key_io.h>
#include <mw/crypto/Hasher.h>
#include <mw/crypto/SecretKeys.h>
#include <mw/models/crypto/Commitment.h>
#include <mw/models/tx/OutputMask.h>
#include <mw/models/tx/Output.h>
#include <mw/models/wallet/Coin.h>
#include <mw/models/wallet/Recipient.h>
#include <mw/models/wallet/StealthAddress.h>
#include <consensus/validation.h>
#include <mw/models/tx/PegOutCoin.h>
#include <mw/wallet/TxBuilder.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <streams.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <version.h>

#include <boost/optional.hpp>
#include <boost/variant/get.hpp>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace oyoltc {
namespace mweb {

namespace {

constexpr const char kDomainScan[]  = "OYO-MWEB-SCAN-v1";
constexpr const char kDomainSpend[] = "OYO-MWEB-SPEND-v1";
constexpr const char kDomainIndex[] = "OYO-MWEB-IDX-v1";

}  // namespace

SecretKey DeriveScanSecret(const std::array<uint8_t, 32>& seed) {
    CSHA256 h;
    h.Write(reinterpret_cast<const uint8_t*>(kDomainScan),
            sizeof(kDomainScan) - 1);
    h.Write(seed.data(), seed.size());
    uint8_t out[32];
    h.Finalize(out);
    return SecretKey(out);
}

SecretKey DeriveSpendSecret(const std::array<uint8_t, 32>& seed) {
    CSHA256 h;
    h.Write(reinterpret_cast<const uint8_t*>(kDomainSpend),
            sizeof(kDomainSpend) - 1);
    h.Write(seed.data(), seed.size());
    uint8_t out[32];
    h.Finalize(out);
    return SecretKey(out);
}

SecretKey DeriveSubSpendKey(const SecretKey& scan_secret,
                            const SecretKey& spend_secret,
                            uint32_t index) {
    CSHA256 h;
    h.Write(reinterpret_cast<const uint8_t*>(kDomainIndex),
            sizeof(kDomainIndex) - 1);
    h.Write(scan_secret.data(), scan_secret.size());
    uint8_t idx_be[4] = {
        static_cast<uint8_t>((index >> 24) & 0xff),
        static_cast<uint8_t>((index >> 16) & 0xff),
        static_cast<uint8_t>((index >> 8) & 0xff),
        static_cast<uint8_t>(index & 0xff),
    };
    h.Write(idx_be, sizeof(idx_be));
    uint8_t mi_bytes[32];
    h.Finalize(mi_bytes);
    SecretKey mi(mi_bytes);
    return SecretKeys::From(spend_secret).Add(mi).Total();
}

StealthPair DeriveStealthAddress(const SecretKey& scan_secret,
                                 const SecretKey& spend_secret,
                                 uint32_t index) {
    SecretKey bi = DeriveSubSpendKey(scan_secret, spend_secret, index);
    PublicKey B = PublicKey::From(bi);
    PublicKey A = B.Mul(scan_secret);
    return {A, B};
}

RewindResult TryRewindOutput(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const SpendPubkeyMap&          spend_pubkey_index,
    const std::array<uint8_t, 33>& commit_bytes,
    const std::array<uint8_t, 33>& ko_bytes,
    const std::vector<uint8_t>&    msg_bytes)
{
    RewindResult r;

    SecretKey  scan(scan_secret_bytes.data());
    PublicKey  Ko(ko_bytes.data());
    Commitment commit(BigInt<33>(commit_bytes.data()));

    OutputMessage message;
    try {
        VectorReader vr(SER_NETWORK, PROTOCOL_VERSION, msg_bytes, /*offset=*/0);
        vr >> message;
    } catch (...) {
        return r;
    }

    // shared_secret = Ke * scan_secret
    PublicKey shared_secret = message.key_exchange_pubkey.Mul(scan);

    // Cheap view-tag pre-filter — most non-ours outputs die here without
    // touching the more expensive arithmetic.
    uint8_t expected_tag = Hashed(EHashTag::TAG, shared_secret)[0];
    if (expected_tag != message.view_tag) return r;

    // t = H_DERIVE(shared_secret); B_i = Ko / H_OUT_KEY(t)
    // v0.21.5.5 libmw made SecretKey no longer implicitly constructible from a
    // Hash; use FromHash so the hash-to-scalar fallback matches Keychain.cpp.
    SecretKey t   = SecretKey::FromHash(Hashed(EHashTag::DERIVE, shared_secret));
    PublicKey B_i = Ko.Div(SecretKey::FromHash(Hashed(EHashTag::OUT_KEY, t)));

    std::array<uint8_t, 33> b_i_bytes;
    std::memcpy(b_i_bytes.data(), B_i.data(), 33);
    auto it = spend_pubkey_index.find(b_i_bytes);
    if (it == spend_pubkey_index.end()) return r;  // not ours

    // Recover plaintext value via the OutputMask and verify against commit.
    OutputMask mask  = OutputMask::FromShared(t);
    uint64_t   value = mask.MaskValue(message.masked_value);
    if (mask.SwitchCommit(value) != commit) return r;

    r.matched         = true;
    r.matched_address = it->second;
    r.amount          = value;
    std::memcpy(r.shared_secret.data(), t.data(), 32);
    return r;
}

namespace {

// Classify a destination string. Stealth addresses go through the MWEB
// recipient path; standard P2PKH/P2SH/P2WPKH/P2WSH addresses route to
// peg-out (a kernel with embedded scriptPubKey; the actual canonical
// LTC vout to that script is materialized by the miner via HogEx).
enum class DestKind { Stealth, PegOut, Invalid };

struct ClassifiedDest {
    DestKind        kind = DestKind::Invalid;
    StealthAddress  stealth;
    CScript         script_pubkey;
};

ClassifiedDest ClassifyDestination(const std::string& addr) {
    ClassifiedDest out;
    CTxDestination dest = DecodeDestination(addr);
    if (!IsValidDestination(dest)) return out;
    if (const StealthAddress* sa = boost::get<StealthAddress>(&dest)) {
        out.kind    = DestKind::Stealth;
        out.stealth = *sa;
        return out;
    }
    out.kind          = DestKind::PegOut;
    out.script_pubkey = GetScriptForDestination(dest);
    return out;
}

}  // namespace

BuildMwebSendResult BuildMwebSendTx(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const std::array<uint8_t, 32>& spend_secret_bytes,
    const std::vector<CoinSpendInfo>& coins,
    const std::vector<SendRecipient>& recipients,
    const std::string& change_address,
    uint64_t change_amount_sat,
    uint64_t caller_fee_budget_sat,
    uint64_t fee_rate_sat_per_vb,
    uint32_t nLockTime)
{
    if (fee_rate_sat_per_vb == 0) fee_rate_sat_per_vb = 1;
    const uint64_t fee_sat = caller_fee_budget_sat;
    if (coins.empty()) throw std::runtime_error("no inputs");
    if (recipients.empty()) throw std::runtime_error("no recipients");

    SecretKey scan(scan_secret_bytes.data());
    SecretKey spend(spend_secret_bytes.data());

    auto build_inputs = [&]() {
        std::vector<mw::Coin> inputs;
        inputs.reserve(coins.size());
        for (const auto& c : coins) {
            SecretKey t(c.shared_secret.data());
            SecretKey b_i = DeriveSubSpendKey(scan, spend, c.address_index);
            SecretKey output_spend_key = SecretKeys::From(b_i)
                .Mul(SecretKey::FromHash(Hashed(EHashTag::OUT_KEY, t)))
                .Total();
            OutputMask mask = OutputMask::FromShared(t);

            mw::Coin coin;
            coin.address_index = c.address_index;
            coin.spend_key     = output_spend_key;
            coin.blind         = mask.GetRawBlind();
            coin.amount        = static_cast<CAmount>(c.amount_sat);
            coin.output_id     = mw::Hash(c.output_id.data());
            coin.shared_secret = t;
            inputs.push_back(std::move(coin));
        }
        return inputs;
    };

    // Classify recipients once; same lambda picks them up for both passes.
    std::vector<ClassifiedDest> classified;
    classified.reserve(recipients.size());
    for (const auto& r : recipients) {
        ClassifiedDest c = ClassifyDestination(r.mweb_address);
        if (c.kind == DestKind::Invalid) {
            throw std::runtime_error("invalid destination: " + r.mweb_address);
        }
        classified.push_back(std::move(c));
    }

    auto build_recipients = [&](uint64_t change_amt) {
        std::vector<mw::Recipient> recv;
        recv.reserve(recipients.size() + 1);
        for (size_t i = 0; i < recipients.size(); ++i) {
            if (classified[i].kind == DestKind::Stealth) {
                recv.push_back(mw::Recipient{
                    static_cast<CAmount>(recipients[i].amount_sat),
                    classified[i].stealth});
            }
            // PegOut entries do NOT contribute to recipients.
        }
        if (change_amt > 0) {
            ClassifiedDest cc = ClassifyDestination(change_address);
            if (cc.kind != DestKind::Stealth) {
                throw std::runtime_error("change address must be a stealth address");
            }
            recv.push_back(mw::Recipient{static_cast<CAmount>(change_amt), cc.stealth});
        }
        return recv;
    };

    auto build_pegouts = [&]() {
        std::vector<PegOutCoin> out;
        for (size_t i = 0; i < recipients.size(); ++i) {
            if (classified[i].kind == DestKind::PegOut) {
                out.emplace_back(static_cast<CAmount>(recipients[i].amount_sat),
                                 classified[i].script_pubkey);
            }
        }
        return out;
    };

    boost::optional<CAmount> pegin_amount = boost::none;
    std::vector<mw::Coin> output_coins_unused;

    // The node's mempool acceptance enforces:
    //   fee >= mweb_weight * BASE_MWEB_FEE (=100)
    //   AND for tx-es with non-zero canonical vsize (peg-out cases):
    //   (fee - mweb_part) >= canonical_vsize * 1 sat/vB (default min relay)
    //
    // For pure MWEB→MWEB the canonical vin/vout are empty AND there are no
    // peg-outs, so GetTransactionWeight() returns 0 → vsize = 0 → only the
    // mweb_weight*100 floor applies. For peg-out tx the wrapper has empty
    // vin/vout but PegOutCoin entries contribute pegout_weight (encoded as
    // CTxOut * WITNESS_SCALE_FACTOR), giving a non-zero vsize that must be
    // covered by the canonical-side fee.
    //
    // Two-pass: build once with the caller's fee budget, observe the actual
    // mweb_weight + canonical vsize, recompute canonical_fee, rebuild with
    // change adjusted to absorb the leftover. The caller's fee_sat is
    // treated as a ceiling — refused if canonical_fee exceeds it.
    constexpr uint64_t kBaseMwebFeePerWeight = 100;

    auto inputs1  = build_inputs();
    auto recv1    = build_recipients(change_amount_sat);
    auto pegouts1 = build_pegouts();
    auto first_pass = TxBuilder::BuildTx(
        inputs1, recv1, pegouts1, pegin_amount,
        static_cast<CAmount>(fee_sat),
        output_coins_unused);

    const uint64_t weight = first_pass->CalcWeight();

    // Compute canonical vsize so we can cover the min-relay floor when
    // pegouts contribute non-zero canonical bytes. For pure MWEB→MWEB this
    // returns 0 (IsMWEBOnly + no pegouts → GetTransactionWeight==0).
    auto compute_canonical_vsize = [](const mw::Transaction::CPtr& mtx) -> uint64_t {
        CMutableTransaction tmp;
        tmp.nVersion = CTransaction::CURRENT_VERSION;
        tmp.mweb_tx  = MWEB::Tx(mtx);
        const int64_t w = ::GetTransactionWeight(CTransaction(tmp));
        // vsize = ceil(weight / WITNESS_SCALE_FACTOR), default scale = 4.
        return static_cast<uint64_t>((w + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR);
    };
    const uint64_t canonical_vsize = compute_canonical_vsize(first_pass);
    const uint64_t canonical_fee   = weight * kBaseMwebFeePerWeight
                                    + canonical_vsize * fee_rate_sat_per_vb;

    mw::Transaction::CPtr mweb_tx;
    uint64_t actual_change = change_amount_sat;
    if (canonical_fee == fee_sat) {
        mweb_tx = first_pass;
    } else {
        if (canonical_fee > fee_sat) {
            // Caller didn't budget enough for the actual weight. Refuse rather
            // than silently use more change → less fee than the network needs.
            throw std::runtime_error(
                "fee_sat too low: canonical=" + std::to_string(canonical_fee) +
                " (weight=" + std::to_string(weight) + " * " +
                std::to_string(kBaseMwebFeePerWeight) + ")");
        }
        // canonical_fee < fee_sat → roll the difference back into change.
        actual_change = change_amount_sat + (fee_sat - canonical_fee);
        auto inputs2  = build_inputs();
        auto recv2    = build_recipients(actual_change);
        auto pegouts2 = build_pegouts();
        std::vector<mw::Coin> output_coins_unused2;
        mweb_tx = TxBuilder::BuildTx(
            inputs2, recv2, pegouts2, pegin_amount,
            static_cast<CAmount>(canonical_fee),
            output_coins_unused2);
    }

    // Wrap in a CMutableTransaction. For an MWEB-only transfer the
    // canonical part has empty vin/vout (CTransaction::IsMWEBOnly).
    // nLockTime is observable on chain even for MWEB-only tx via the
    // canonical envelope — set it for fingerprint parity with node-wallet.
    CMutableTransaction mtx;
    mtx.nVersion  = CTransaction::CURRENT_VERSION;
    mtx.nLockTime = nLockTime;
    mtx.mweb_tx   = MWEB::Tx(mweb_tx);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << mtx;

    BuildMwebSendResult res;
    res.tx_hex         = HexStr(ss);
    res.actual_fee_sat = canonical_fee;
    res.change_sat     = actual_change;
    res.weight         = weight;
    return res;
}

BuildPegInResult BuildPegInMwebPart(
    const std::string& recipient_stealth_addr,
    uint64_t           recipient_amount_sat,
    const std::string& change_stealth_addr,
    uint64_t           change_amount_sat,
    uint64_t           caller_mweb_fee_budget_sat)
{
    if (recipient_amount_sat == 0) throw std::runtime_error("recipient_amount_sat must be > 0");

    ClassifiedDest recv_dest = ClassifyDestination(recipient_stealth_addr);
    if (recv_dest.kind != DestKind::Stealth) {
        throw std::runtime_error("peg-in destination must be a stealth address");
    }

    StealthAddress change_stealth;
    if (change_amount_sat > 0) {
        ClassifiedDest cd = ClassifyDestination(change_stealth_addr);
        if (cd.kind != DestKind::Stealth) {
            throw std::runtime_error("peg-in change address must be a stealth address");
        }
        change_stealth = cd.stealth;
    }

    constexpr uint64_t kBaseMwebFeePerWeight = 100;

    // No MWEB inputs; peg-in feeds value into the MWEB through the kernel's
    // pegin field. Recipients = [external] (+ [own change] if change_amount>0).
    // Build once with caller budget to learn the weight, then rebuild with
    // canonical mweb_fee = weight * 100.
    std::vector<mw::Coin>      input_coins;
    std::vector<PegOutCoin>    pegouts;

    auto build_recipients = [&]() {
        std::vector<mw::Recipient> rec{
            mw::Recipient{static_cast<CAmount>(recipient_amount_sat), recv_dest.stealth}};
        if (change_amount_sat > 0) {
            rec.push_back(mw::Recipient{
                static_cast<CAmount>(change_amount_sat), change_stealth});
        }
        return rec;
    };

    auto build = [&](uint64_t mweb_fee) {
        const uint64_t pegin = recipient_amount_sat + change_amount_sat + mweb_fee;
        boost::optional<CAmount> pegin_opt =
            boost::make_optional<CAmount>(static_cast<CAmount>(pegin));
        std::vector<mw::Coin> out_unused;
        std::vector<mw::Recipient> rec = build_recipients();
        return TxBuilder::BuildTx(
            input_coins, rec, pegouts, pegin_opt,
            static_cast<CAmount>(mweb_fee), out_unused);
    };

    auto first_pass = build(caller_mweb_fee_budget_sat);
    const uint64_t weight        = first_pass->CalcWeight();
    const uint64_t canonical_fee = weight * kBaseMwebFeePerWeight;

    mw::Transaction::CPtr mweb_tx = first_pass;
    if (canonical_fee != caller_mweb_fee_budget_sat) {
        if (canonical_fee > caller_mweb_fee_budget_sat) {
            throw std::runtime_error(
                "mweb fee too low: canonical=" + std::to_string(canonical_fee) +
                " caller_max=" + std::to_string(caller_mweb_fee_budget_sat));
        }
        mweb_tx = build(canonical_fee);
    }

    if (mweb_tx->GetKernels().empty()) {
        throw std::runtime_error("peg-in mweb tx has no kernels");
    }
    const Kernel& k = mweb_tx->GetKernels().front();

    BuildPegInResult res;
    res.mweb_tx             = mweb_tx;
    res.pegin_amount_sat    = recipient_amount_sat + change_amount_sat + canonical_fee;
    res.actual_mweb_fee_sat = canonical_fee;
    res.mweb_weight         = weight;
    {
        const auto& kid = k.GetKernelID();
        std::memcpy(res.kernel_id.data(), kid.data(), 32);
    }
    return res;
}

RewindResult TryRewindOutputBlob(
    const std::array<uint8_t, 32>& scan_secret_bytes,
    const SpendPubkeyMap&          spend_pubkey_index,
    const std::vector<uint8_t>&    output_blob,
    std::array<uint8_t, 33>*       out_commit,
    std::array<uint8_t, 32>*       out_output_id)
{
    RewindResult r;
    if (output_blob.empty()) return r;

    Output output;
    try {
        VectorReader vr(SER_NETWORK, PROTOCOL_VERSION, output_blob, /*offset=*/0);
        vr >> output;
    } catch (...) { return r; }

    const Commitment& commit = output.GetCommitment();
    const PublicKey&  Ko     = output.GetReceiverPubKey();

    std::array<uint8_t, 33> commit_bytes;
    std::array<uint8_t, 33> ko_bytes;
    std::memcpy(commit_bytes.data(), commit.data(), 33);
    std::memcpy(ko_bytes.data(),     Ko.data(),     33);
    std::vector<uint8_t> msg_bytes = output.GetOutputMessage().Serialized();

    r = TryRewindOutput(scan_secret_bytes, spend_pubkey_index,
                        commit_bytes, ko_bytes, msg_bytes);
    if (r.matched) {
        if (out_commit)    *out_commit    = commit_bytes;
        if (out_output_id) {
            const auto& oid = output.GetOutputID();
            std::memcpy(out_output_id->data(), oid.data(), 32);
        }
    }
    return r;
}

}  // namespace mweb
}  // namespace oyoltc

// ---------------------------------------------------------------------------
// C ABI: smoke test exported for Go cgo to validate libmw linkage.
// ---------------------------------------------------------------------------

namespace {

// Thread-local storage for the borrowed JSON pointer returned to caller.
// Lives until the next call on the same thread, matching the borrowed-buffer
// contract used elsewhere in liboyoltc.
thread_local std::string g_self_test_cache;

// Fixed test seed (no RNG): deterministic outputs let the Go test assert
// exact hex values. Bytes are ASCII for readability; payload meaning is none.
constexpr std::array<uint8_t, 32> kTestSeed = {
    'o','y','o','-','m','w','e','b','-','t','e','s','t','-','s','e',
    'e','d','-','v','0',0,0,0,0,0,0,0,0,0,0,0,
};

}  // namespace

extern "C" int32_t oyo_mweb_self_test(OYO_CTX /*ctx*/,
                                      const uint8_t** out_json,
                                      size_t* out_len) {
    if (!out_json || !out_len) return OYO_ERR_INVALID_ARG;
    try {
        SecretKey scan  = oyoltc::mweb::DeriveScanSecret(kTestSeed);
        SecretKey spend = oyoltc::mweb::DeriveSpendSecret(kTestSeed);
        PublicKey scan_pub  = PublicKey::From(scan);
        PublicKey spend_pub = PublicKey::From(spend);
        auto sa0 = oyoltc::mweb::DeriveStealthAddress(scan, spend, 0);

        UniValue out(UniValue::VOBJ);
        out.pushKV("status", "ok");
        out.pushKV("scan_pubkey",  scan_pub.ToHex());
        out.pushKV("spend_pubkey", spend_pub.ToHex());
        out.pushKV("stealth_a", sa0.scan.ToHex());
        out.pushKV("stealth_b", sa0.spend.ToHex());
        g_self_test_cache = out.write();

        *out_json = reinterpret_cast<const uint8_t*>(g_self_test_cache.data());
        *out_len  = g_self_test_cache.size();
        return OYO_OK;
    } catch (...) {
        return OYO_ERR_INTERNAL;
    }
}
