#!/usr/bin/env bash
#
# Assemble the portable OYO release archive (the final pipeline step).
#
# Produces  dist/out/oyo-portable-<version>-linux-x86_64.tar.gz  containing:
#     oyo-portable-<version>/
#       ├─ oyo.sh          installer / manager (full|web|node)
#       ├─ README.md
#       ├─ VERSION
#       ├─ node/litecoind  (static, stripped — only baseline shared libs)
#       └─ web/oyo-web     (stripped, static libcrypto — same baseline deps)
#
# Assemble-only: expects the artifacts to be already built —
#   litecoin-oyo image              the tested portable litecoind
#                                   (scripts/06 or scripts/ci-ensure.sh)
#   scripts/04-build-liboyoltc.sh   cgo bundle (input of scripts/05)
#   scripts/05-build-oyo-web.sh     dist/out/oyo-web
#
# litecoind is taken from the node runtime image, not a build volume: the
# image exists on cache hits too, and it is the exact binary the e2e suite
# ran against.
#
# Usage:  ./scripts/07-package.sh [version]    (version defaults to git describe)
#
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT_DIR="$(pwd)"
NODE_IMAGE=litecoin-oyo

VERSION="${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
NAME="oyo-portable-$VERSION"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist/out}"
TARBALL="$OUT_DIR/$NAME-linux-x86_64.tar.gz"

say() { printf '\n\033[1;33m== %s ==\033[0m\n' "$*"; }

say "1/4  preflight"
test -f dist/out/oyo-web || {
  echo "no dist/out/oyo-web — run scripts/05-build-oyo-web.sh first" >&2
  exit 1
}
docker image inspect "$NODE_IMAGE:latest" >/dev/null 2>&1 || {
  echo "no $NODE_IMAGE image — run scripts/06-build-node-image.sh (or scripts/ci-ensure.sh) first" >&2
  exit 1
}
echo "   artifacts present"

say "2/4  assemble $NAME/"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"
mkdir -p "$PKG/node" "$PKG/web"
# litecoind: copy out of the node image (already stripped by scripts/06)
docker run --rm -u "$(id -u):$(id -g)" \
  -v "$PKG/node:/out" --entrypoint /bin/bash "$NODE_IMAGE" \
  -c 'cp /usr/local/bin/litecoind /out/litecoind && chmod +x /out/litecoind'
install -m 0755 dist/out/oyo-web "$PKG/web/oyo-web"
install -m 0755 dist/oyo.sh      "$PKG/oyo.sh"
install -m 0644 dist/README.md   "$PKG/README.md"
printf '%s\n' "$VERSION" > "$PKG/VERSION"

say "3/4  verify portability (only baseline shared libs allowed)"
for b in "$PKG/node/litecoind" "$PKG/web/oyo-web"; do
  # baseline = glibc (incl. its split-out libpthread/libdl/librt/libresolv on
  # pre-2.34 hosts) + libstdc++/libgcc runtime, present on every Linux
  bad="$(ldd "$b" 2>/dev/null | grep -ivE 'linux-vdso|ld-linux|libc\.so|libm\.so|libstdc\+\+\.so|libgcc_s\.so|libpthread\.so|libdl\.so|librt\.so|libresolv\.so' | grep '=>' || true)"
  [ -z "$bad" ] || { echo "ERROR: $b has unexpected deps:"; echo "$bad"; exit 1; }
  printf '   %-22s %s  (deps ok)\n' "$(basename "$b")" "$(du -h "$b" | cut -f1)"
done

say "4/4  archive"
mkdir -p "$OUT_DIR"
tar -C "$STAGE" -czf "$TARBALL" "$NAME"
( cd "$OUT_DIR" && sha256sum "$(basename "$TARBALL")" > "$(basename "$TARBALL").sha256" )

echo
echo "release: $TARBALL"
echo "sha256 : $(cut -d' ' -f1 "$TARBALL.sha256")"
echo "size   : $(du -h "$TARBALL" | cut -f1)"
