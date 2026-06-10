#!/usr/bin/env bash
#
# Ensure the two expensive pipeline images exist locally, reusing
# content-addressed registry tags instead of rebuilding:
#
#   litecoin-oyo-build  (scripts/01: toolchain + BDB + baked depends/)
#       tag deps-<h>,  h = hash(Dockerfile.build-image,
#                              submodule contrib/install_db4.sh, depends/)
#   litecoin-oyo        (scripts/02+03+06: tested portable litecoind)
#       tag node-<h>,  h = hash(deps tag, submodule tree,
#                              scripts/02|03|06, Dockerfile.node)
#
# Resolution order per image: local content tag → registry (pull) → build.
# The node image is built through scripts/03 (make check + functional), so
# a published node-<h> certifies that exact node source passed its tests —
# node tests run exactly when the node changed, by construction.
#
# Usage:  scripts/ci-ensure.sh [--push] [--print-tags]
#   --push        push freshly built tags to the registry (CI; needs
#                 docker login to $OYO_REGISTRY)
#   --print-tags  print the computed tags and exit
#
set -euo pipefail

cd "$(dirname "$0")/.."

REGISTRY="${OYO_REGISTRY:-ghcr.io/pidmitrius}"
PUSH=0
PRINT=0
for arg in "$@"; do
  case "$arg" in
    --push) PUSH=1 ;;
    --print-tags) PRINT=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

short() { sha256sum | cut -c1-12; }

DEPS_H=$( {
  git hash-object docker/Dockerfile.build-image
  git -C litecoin-oyo-fork rev-parse HEAD:contrib/install_db4.sh HEAD:depends
} | short )
DEPS_TAG="deps-$DEPS_H"

NODE_H=$( {
  echo "$DEPS_TAG"
  git -C litecoin-oyo-fork rev-parse 'HEAD^{tree}'
  git hash-object scripts/02-build-litecoind.sh scripts/03-test-litecoind.sh \
                  scripts/06-build-node-image.sh docker/Dockerfile.node
} | short )
NODE_TAG="node-$NODE_H"

if [ "$PRINT" = 1 ]; then
  echo "DEPS_TAG=$DEPS_TAG"
  echo "NODE_TAG=$NODE_TAG"
  exit 0
fi

say() { printf '\n\033[1;33m== %s ==\033[0m\n' "$*"; }

# ensure <local-name> <registry-repo> <tag> <build-cmd...>
# Leaves the image available as BOTH <local-name>:latest (what the numbered
# scripts use) and <local-name>:<tag> (the content-addressed marker).
ensure() {
  local name="$1" repo="$2" tag="$3"; shift 3
  if docker image inspect "$name:$tag" >/dev/null 2>&1; then
    say "$name:$tag — local hit"
    docker tag "$name:$tag" "$name:latest"
    return 0
  fi
  if docker manifest inspect "$repo:$tag" >/dev/null 2>&1; then
    say "$name:$tag — registry hit, pulling"
    docker pull "$repo:$tag"
    docker tag "$repo:$tag" "$name:$tag"
    docker tag "$repo:$tag" "$name:latest"
    return 0
  fi
  say "$name:$tag — miss, building"
  "$@"
  docker tag "$name:latest" "$name:$tag"
  if [ "$PUSH" = 1 ]; then
    docker tag "$name:latest" "$repo:$tag"
    docker push "$repo:$tag"
  fi
}

build_node() {
  ./scripts/02-build-litecoind.sh
  ./scripts/03-test-litecoind.sh
  ./scripts/06-build-node-image.sh
}

ensure litecoin-oyo-build "$REGISTRY/oyo-ltc-build" "$DEPS_TAG" ./scripts/01-build-image.sh
ensure litecoin-oyo       "$REGISTRY/oyo-ltc-node"  "$NODE_TAG" build_node

say "images ready: litecoin-oyo-build:$DEPS_TAG, litecoin-oyo:$NODE_TAG"
