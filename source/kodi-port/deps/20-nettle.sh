#!/bin/bash
# Nettle 3.7.3 (Version wie Kodi 21.3) mit hogweed (Public-Key, braucht GMP aus 19-gmp) für
# gnutls. Patch und Optionen wie Kodis tools/depends/target/nettle: ohne Testsuite, ohne
# Laufzeit-CPU-Weiche (fat), ohne OpenSSL-Vergleichsbenchmarks, ohne Doku. CCPIC=" " wie bei
# Kodi; -fPIC kommt ohnehin aus unseren CFLAGS.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done nettle && exit 0
fetch nettle
cd "$S"
patch -p1 -i "$KODI_SRC/tools/depends/target/nettle/01-disable_testsuite.patch"
autoreconf >/dev/null
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
  --disable-fat --disable-openssl --disable-documentation CCPIC=" "
make -j"$JOBS"
make install
for l in nettle hogweed; do
  [ -f "$PREFIX/lib/lib$l.a" ] && [ -f "$PREFIX/lib/pkgconfig/$l.pc" ] || { echo "FEHLER: lib$l fehlt"; exit 1; }
done
done_stamp nettle
