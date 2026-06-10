#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

docker run --rm -i \
  -e JOBS="${JOBS:-}" \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  -v "$(pwd):/src:ro" \
  -v "$(pwd)/liboyoltc:/out" \
  -v oyo-ltc-node-work:/work \
  litecoin-oyo-build \
  bash -s <<'SCRIPT'
set -euo pipefail

src=/src/litecoin-oyo-fork
libsrc=/src/liboyoltc/src
work=/work/litecoin-oyo-fork
jobs="${JOBS:-$(nproc)}"

if [ ! -d "$src" ]; then
  echo "missing submodule at $src" >&2
  exit 1
fi
if [ ! -d "$libsrc" ]; then
  echo "missing liboyoltc sources at $libsrc" >&2
  exit 1
fi

echo "=== Syncing litecoin source ==="
mkdir -p "$work"
rsync -a --checksum --exclude=".git" "$src/" "$work/"

echo "=== Overlaying liboyoltc source ==="
mkdir -p "$work/src/oyoltc"
rsync -a --checksum "$libsrc/" "$work/src/oyoltc/"

cd "$work"

if ! grep -q '^LIBOYOLTC=liboyoltc\.a$' src/Makefile.am; then
  sed -i '/^LIBMW=libmw\.a$/a LIBOYOLTC=liboyoltc.a' src/Makefile.am
fi

if ! grep -q '$(LIBOYOLTC)' src/Makefile.am; then
  awk '
    BEGIN { in_extra = 0 }
    /^EXTRA_LIBRARIES \+= \\/ { in_extra = 1; print; next }
    in_extra && /^[[:space:]]*\$\(LIBMW\)$/ {
      print "  $(LIBMW) \\"
      print "  $(LIBOYOLTC)"
      in_extra = 0
      next
    }
    { print }
  ' src/Makefile.am > src/Makefile.am.new
  mv src/Makefile.am.new src/Makefile.am
fi

if ! grep -q '^# liboyoltc - external-wallet backend for Go consumers via cgo\.$' src/Makefile.am; then
  awk '
    /^\.PHONY: FORCE check-symbols check-security$/ {
      print "# liboyoltc - external-wallet backend for Go consumers via cgo."
      print "# Public ABI in src/oyoltc/oyoltc.h. Logic (chain sync, UTXO tracking,"
      print "# reorg rollback, balances, rescan) lives here; Go side is a transport wrapper."
      print "LIBOYOLTC_CPPFLAGS = \\"
      print "  -I$(srcdir) \\"
      print "  -I$(srcdir)/secp256k1-zkp/include \\"
      print "  -I$(srcdir)/libmw/include \\"
      print "  -I$(srcdir)/libmw/deps/crypto/include"
      print "liboyoltc_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES) $(LIBOYOLTC_CPPFLAGS)"
      print "liboyoltc_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -fvisibility=hidden"
      print "liboyoltc_a_SOURCES = \\"
      print "\toyoltc/oyoltc.cpp \\"
      print "\toyoltc/oyoltc.h \\"
      print "\toyoltc/mweb.cpp \\"
      print "\toyoltc/mweb.h \\"
      print "\toyoltc/stubs.cpp"
      print ""
      print "noinst_PROGRAMS += oyoltc-probe"
      print "oyoltc_probe_SOURCES = oyoltc/probe.cpp oyoltc/stubs.cpp"
      print "oyoltc_probe_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)/oyoltc"
      print "oyoltc_probe_CXXFLAGS = $(AM_CXXFLAGS)"
      print "oyoltc_probe_LDADD = \\"
      print "  $(LIBOYOLTC) \\"
      print "  $(LIBLEVELDB) \\"
      print "  $(LIBMW) \\"
      print "  $(LIBBITCOIN_COMMON) \\"
      print "  $(LIBBITCOIN_CONSENSUS) \\"
      print "  $(LIBBITCOIN_UTIL) \\"
      print "  $(LIBUNIVALUE) \\"
      print "  $(LIBBITCOIN_CRYPTO) \\"
      print "  $(LIBSECP256K1) \\"
      print "  $(BOOST_LIBS) \\"
      print "  $(CRYPTO_LIBS) \\"
      print "  $(MWEB_LIBS) \\"
      print "  $(SQLITE_LIBS)"
      print ""
    }
    { print }
  ' src/Makefile.am > src/Makefile.am.new
  mv src/Makefile.am.new src/Makefile.am
fi

echo "=== autogen ==="
./autogen.sh

echo "=== configure ==="
export BDB_PREFIX=/opt/db4
./configure --without-gui --disable-bench \
  BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" \
  BDB_CFLAGS="-I${BDB_PREFIX}/include"

echo "=== build liboyoltc ==="
make -C src -j"$jobs" liboyoltc.a oyoltc-probe

echo "=== extracting archives ==="
mkdir -p /out/include /out/lib
cp src/oyoltc/oyoltc.h /out/include/oyoltc.h
cp src/liboyoltc.a /out/lib/
cp src/leveldb/libleveldb.a /out/lib/
cp src/libmw.a /out/lib/
cp src/libbitcoin_common.a /out/lib/
cp src/libbitcoin_consensus.a /out/lib/
cp src/libbitcoin_util.a /out/lib/
cp src/crypto/libbitcoin_crypto_base.a /out/lib/
for f in libcrc32c.a libcrc32c_sse42.a; do
  [ -f "src/crc32c/$f" ] && cp "src/crc32c/$f" /out/lib/ || true
done
cp src/univalue/.libs/libunivalue.a /out/lib/
cp src/secp256k1-zkp/.libs/libsecp256k1.a /out/lib/
for f in \
    src/crypto/libbitcoin_crypto_sse41.a \
    src/crypto/libbitcoin_crypto_avx2.a \
    src/crypto/libbitcoin_crypto_shani.a; do
  [ -f "$f" ] && cp "$f" /out/lib/ || true
done
for f in libfmt.a libboost_filesystem.a libboost_thread.a libboost_system.a libcrypto.a; do
  [ -f "/usr/lib/x86_64-linux-gnu/$f" ] && cp "/usr/lib/x86_64-linux-gnu/$f" /out/lib/ || true
done

echo "=== merging liboyoltc_bundle.a ==="
cd /out/lib
rm -f liboyoltc_bundle.a

mri="CREATE liboyoltc_bundle.a"
for f in \
    liboyoltc.a \
    libleveldb.a \
    libmw.a \
    libbitcoin_common.a \
    libbitcoin_consensus.a \
    libbitcoin_util.a \
    libunivalue.a \
    libbitcoin_crypto_base.a \
    libcrc32c.a \
    libcrc32c_sse42.a \
    libbitcoin_crypto_sse41.a \
    libbitcoin_crypto_avx2.a \
    libbitcoin_crypto_shani.a \
    libsecp256k1.a; do
  [ -f "$f" ] && mri="${mri}
ADDLIB $f"
done
mri="${mri}
SAVE
END"
printf '%s\n' "$mri" | ar -M

rm -f oyoltc-stubs.o
ar x liboyoltc.a liboyoltc_a-stubs.o
mv liboyoltc_a-stubs.o oyoltc-stubs.o

ls -lh liboyoltc_bundle.a oyoltc-stubs.o
chown -R "$HOST_UID:$HOST_GID" /out/include /out/lib
SCRIPT
