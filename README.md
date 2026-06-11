# OYO — Own Your Own implementation for Litecoin

OYO is a self-contained wallet and explorer stack for Litecoin, with
first-class MWEB (MimbleWimble Extension Block) support. One portable Go
binary — `oyo-web` — embeds a C++ wallet engine (`liboyoltc`) and a
single-page UI, talks to a litecoind over JSON-RPC, and gives you full
command of the network: HD wallets, stealth addresses, peg-in/peg-out,
coin control, a transaction queue, and a local chain explorer.

## The OYO contract

**Own Your Own** is the design rule everything here follows:

- **Own your keys.** OYO wallets are derived deterministically from a seed
  you supply and exist only in process memory. Nothing sensitive ever
  touches disk — no wallet files, no key stores, no seed cache. Restart the
  process and you hold the only thing that matters: the seed. Supply it
  again and the wallet reassembles, balances included, from the local chain
  mirror.
- **Own your data.** The engine maintains its own UTXO mirror of the whole
  chain — regular and MWEB — in local LevelDB, built from raw blocks your
  node serves. Every wallet operation and every explorer lookup is answered
  from that mirror. Your address set never leaves the process: no
  address-indexed node RPCs, no third-party explorers, no telemetry, no
  external calls at all (even the API-docs UI assets are bundled).
- **Own your node.** The node is just a block source and a broadcaster. A
  stock litecoind works; the thin fork in this repo adds optional
  node-wallet conveniences, and oyo-web detects their absence and degrades
  gracefully.
- **Own your transactions.** What you approve is what is broadcast: the
  confirm flow signs once, shows the exact breakdown (inputs, outputs,
  change, fee, mempool acceptance), and broadcasts those bytes verbatim.
  Branch-and-bound coin selection prefers changeless transactions, avoiding
  the change-output fingerprint that ties spends together on-chain.

## What you can do

- **Wallets**: regular (canonical P2WPKH, plus legacy and nested-SegWit
  address kinds), MWEB-only (stealth addresses), universal (canonical +
  MWEB under one seed), watch-only — all in-memory OYO wallets; plus
  litecoind's own node wallets managed through the same UI and API.
- **The full send matrix**: canonical→canonical, peg-in (canonical→MWEB),
  pure MWEB→MWEB, peg-out (MWEB→canonical). Multi-recipient sends,
  send-all/Max per side, manual UTXO selection, custom change address, fee
  presets from the node's estimator or an explicit rate.
- **Queue**: store send *recipes* (intent, not signed bytes) and run them
  manually or on a schedule; each task is re-built and re-signed against
  current wallet state at execution time, and fails cleanly if the recipe
  can no longer be satisfied.
- **Explorer**: blocks, transactions, mempool, and the history of any
  address — including MWEB stealth output history — answered locally from
  the mirror, with an optional node cross-check.
- **Sync**: pipelined block prefetch (thousands of blocks per minute
  against a local node), readiness gating for sends, deep-reorg safety,
  progress and ETA in the UI.

## REST API

Everything the UI does goes through the REST API — the frontend is just
another client. The surface covers wallet lifecycle (create / load /
unload / delete), balances and per-address breakdowns, UTXO listings,
address generation, transaction history, dry-run `estimate-send` returning
the exact signed-transaction breakdown, a confirm-token `send` that
broadcasts the previewed bytes verbatim, queue management, chain and
mempool sync drives, explorer lookups with smart search, fee estimation,
and node introspection. Optional HTTP Basic auth (plus cookie sessions for
the browser) with brute-force throttling makes it safe behind a reverse
proxy, and the recipe queue plus confirm flow make it comfortable to drive
from scripts.

The API is specified in OpenAPI 3.0 (`oyo-web/openapi.yaml`) and served
with a self-hosted Swagger UI at `/api-docs/` — try-it-out works against
the running instance, and the served spec rewrites its server URL from the
forwarding headers, so it stays correct behind a proxy prefix.
Spec↔handler parity is enforced by tests.

## Repository layout

- `litecoin-oyo-fork/` — submodule, a thin Litecoin fork (upstream release
  tag + a small set of node wallet extensions; OYO works with a stock
  litecoind too, with reduced node-wallet features)
- `liboyoltc/` — the C++ wallet engine (chain mirror, UTXO tracking, MWEB,
  send) exposed as a C ABI; `include/` and `lib/` are generated
- `oyo-web/` — Go web app (wallet + explorer SPA) over the engine via cgo
- `docker/` — Dockerfiles (build environment, node runtime image)
- `scripts/` — the build pipeline (below)
- `dist/` — release archive payload (installer `oyo.sh`, archive README);
  artifacts land in `dist/out/`

## Pipeline

| # | script | output |
|---|--------|--------|
| 01 | `scripts/01-build-image.sh` | `litecoin-oyo-build` docker image (toolchain, BDB 4.8, baked depends/, pinned python deps) |
| 02 | `scripts/02-build-litecoind.sh` | portable static `litecoind` (linked against the image's depends/ prefix) |
| 03 | `scripts/03-test-litecoind.sh` | node tests: `make check` + functional smoke |
| 04 | `scripts/04-build-liboyoltc.sh` | cgo bundle → `liboyoltc/{include,lib}` |
| 05 | `scripts/05-build-oyo-web.sh` | portable stripped `oyo-web` → `dist/out/` |
| 06 | `scripts/06-build-node-image.sh` | `litecoin-oyo` node runtime image (for the dev/e2e stand) |
| 07 | `scripts/07-package.sh [version]` | `dist/out/oyo-portable-<version>-linux-x86_64.tar.gz` + sha256 |

A release is `01 → 02 → 04 → 05 → 07`. Both shipped binaries are verified
to depend only on baseline shared libs (glibc, libstdc++/libgcc) — the
archive runs on any reasonably recent x86_64 Linux.

CI reuses the two expensive images through content-addressed registry tags
(`scripts/ci-ensure.sh`): the build image is keyed by its inputs, and the
node image is published only after the node test suite passes — so a
cached tag *is* the proof that source was tested.

## Development

`oyo-web/oyo-dev.sh` is the dev harness:

- `./oyo-dev.sh quick` — go build + go tests, fresh regtest container,
  Playwright e2e suite (rebuilds the cgo bundle automatically when
  `liboyoltc/src` changed)
- `./oyo-dev.sh full` — additionally rebuilds litecoind (02), the bundle
  (04), the node image (06) and runs node tests (03) first

Requirements: docker, Go, node/npm (e2e), python3.
