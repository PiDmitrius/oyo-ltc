# oyo-ltc

OYO — a Litecoin wallet/explorer stack:

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
| 01 | `scripts/01-build-image.sh` | `litecoin-oyo-build` docker image (toolchain, BDB 4.8, pinned python deps) |
| 02 | `scripts/02-build-litecoind.sh` | portable static `litecoind` (depends/-built, cached on the `oyo-ltc-node-static` volume) |
| 03 | `scripts/03-test-litecoind.sh` | node tests: `make check` + functional smoke |
| 04 | `scripts/04-build-liboyoltc.sh` | cgo bundle → `liboyoltc/{include,lib}` |
| 05 | `scripts/05-build-oyo-web.sh` | portable stripped `oyo-web` → `dist/out/` |
| 06 | `scripts/06-build-node-image.sh` | `litecoin-oyo` node runtime image (for the dev/e2e stand) |
| 07 | `scripts/07-package.sh [version]` | `dist/out/oyo-portable-<version>-linux-x86_64.tar.gz` + sha256 |

A release is `01 → 02 → 04 → 05 → 07`. Both shipped binaries are verified
to depend only on baseline shared libs (glibc, libstdc++/libgcc) — the
archive runs on any reasonably recent x86_64 Linux.

## Development

`oyo-web/oyo-dev.sh` is the dev harness:

- `./oyo-dev.sh quick` — go build + go tests, fresh regtest container,
  Playwright e2e suite (rebuilds the cgo bundle automatically when
  `liboyoltc/src` changed)
- `./oyo-dev.sh full` — additionally rebuilds litecoind (02), the bundle
  (04), the node image (06) and runs node tests (03) first

Requirements: docker, Go, node/npm (e2e), python3.
