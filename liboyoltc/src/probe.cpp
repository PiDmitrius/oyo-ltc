#include "oyoltc.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fail(const char* msg) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; }

static void report_err(OYO_CTX ctx, const char* where) {
    int32_t ec = 0; const uint8_t* em = nullptr; size_t el = 0;
    oyo_last_error(ctx, &ec, &em, &el);
    std::fprintf(stderr, "%s err %d: %.*s\n", where, ec, int(el), em ? (const char*)em : "");
}

int main() {
    if (oyo_version() != OYOLTC_ABI_VERSION) return fail("version mismatch");

    OYO_CTX ctx = nullptr;
    if (oyo_open(nullptr, &ctx) != OYO_OK) return fail("oyo_open");

    const char* ccfg = "{\"network\":\"regtest\",\"rollback_window\":100}";
    OYO_CHAIN chain = nullptr;
    if (oyo_chain_open(ctx, (const uint8_t*)ccfg, std::strlen(ccfg), &chain) != OYO_OK) {
        report_err(ctx, "chain_open"); return fail("chain_open");
    }

    const char* wjson = "{\"name\":\"probe\",\"seed_type\":\"oyo_v1\",\"seed\":\"probe-seed\",\"address_count\":3,\"address_kind\":\"p2wpkh\"}";
    OYO_WALLET w = nullptr;
    if (oyo_wallet_open(chain, OYO_WALLET_KIND_REGULAR, (const uint8_t*)wjson, std::strlen(wjson), &w) != OYO_OK) {
        report_err(ctx, "wallet_open"); return fail("wallet_open");
    }

    // Fresh wallet: revision > 0 (bump per binding attach), balance 0.
    uint64_t rev0 = 0;
    if (oyo_wallet_revision(w, &rev0) != OYO_OK) return fail("wallet_revision");
    std::fprintf(stdout, "initial revision: %llu\n", (unsigned long long)rev0);

    const uint8_t* j = nullptr; size_t jl = 0; uint64_t rev = 0;
    if (oyo_wallet_status_since(w, 0, &rev, &j, &jl) != OYO_OK) return fail("status_since(0)");
    std::fprintf(stdout, "wallet status: %.*s\n", int(jl), (const char*)j);

    // Same revision -> OYO_NO_CHANGE.
    int32_t rc = oyo_wallet_status_since(w, rev, &rev, &j, &jl);
    if (rc != OYO_NO_CHANGE) { std::fprintf(stderr, "expected NO_CHANGE, got %d\n", rc); return fail("no_change"); }
    std::fprintf(stdout, "no-change path ok (rc=%d, rev=%llu)\n", rc, (unsigned long long)rev);

    if (oyo_chain_status(chain, &j, &jl) != OYO_OK) return fail("chain_status");
    std::fprintf(stdout, "chain status: %.*s\n", int(jl), (const char*)j);

    OYO_OP op = nullptr;
    if (oyo_chain_sync(chain, &op) != OYO_OK) return fail("chain_sync");
    if (oyo_op_rpc_request(op, &j, &jl) != OYO_OK) return fail("op_rpc_request");
    std::fprintf(stdout, "first sync rpc: %.*s\n", int(jl), (const char*)j);

    oyo_op_close(op);
    oyo_wallet_close(w);
    oyo_chain_close(chain);
    oyo_close(ctx);
    std::fprintf(stdout, "probe OK\n");
    return 0;
}
