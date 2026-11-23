#!/bin/bash
# GnuTLS 3.7.8 (Version wie Kodi 21.3) für libsmbclient (Samba braucht es für seine Krypto, nicht
# für TLS). Patches und Optionen wie Kodis tools/depends/target/gnutls: nettle/hogweed/gmp aus
# 19/20, libtasn1/libunistring/libopts eingebaut, ohne p11-kit, NLS, IDN, Werkzeuge, Doku, Tests.
# add-dl-as-private-lib.patch ändert configure.ac (darum autoreconf), android-fpending.patch
# betrifft nur Android, schadet aber nicht (Kodi wendet alle drei immer an).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done gnutls && exit 0
# autoreconf ruft autopoint auf, weil configure.ac AM_GNU_GETTEXT_VERSION enthält (auch mit
# --disable-nls). autopoint (gettext-Werkzeuge) steht nicht in der Basis-Paketliste, und gnutls
# ist das erste Paket hier, das es braucht.
if ! command -v autopoint >/dev/null; then
  echo "Host-Paket installieren: autopoint"
  sudo -n apt-get install -y -q autopoint
fi
fetch gnutls
cd "$S"
K=$KODI_SRC/tools/depends/target/gnutls
for p in add-dl-as-private-lib 03-support-correct-cisdigit android-fpending; do
  patch -p1 -i "$K/$p.patch"
done
# gtkdocize (gtk-doc-tools) braucht es nur für die Doku; gtk-doc.make und m4/gtk-doc.m4 liegen
# schon im Archiv, darum autoreconf den Aufruf überspringen lassen statt das Paket zu installieren.
# Ausgabe nicht verwerfen: build-deps.sh schreibt sie ins Log, sonst wäre ein Abbruch hier (etwa
# ein fehlendes Werkzeug) dort nicht zu erkennen.
GTKDOCIZE=true autoreconf -vif
# ac_cv_func_*_0_nonnull wie bei Kodi: im Cross-Build kann configure malloc(0)/realloc(0) nicht
# prüfen und nähme sonst die gnulib-Ersatzfunktionen (bei Kodi fehlten deren rpl_*-Symbole
# später beim Linken, siehe Prüfung unten).
# -Wno-error=implicit-int wie bei Kodi (Clang >= 15 macht daraus sonst einen Fehler).
CFLAGS="$CFLAGS -Wno-error=implicit-int" \
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
  --without-p11-kit --disable-nls --with-included-unistring --with-included-libtasn1 \
  --enable-local-libopts --disable-doc --disable-tests --disable-guile --disable-tools --without-idn \
  ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes
make -j"$JOBS"
make install
grep -q -- '-ldl' "$PREFIX/lib/pkgconfig/gnutls.pc" || echo "WARNUNG: -ldl fehlt in gnutls.pc"
# gnulib ersetzt auf 32 Bit trotzdem malloc/realloc (PTRDIFF_MAX-Schutz) und free (errno, glibc
# < 2.33) durch rpl_*. Das ist richtig, solange die Ersatzfunktionen in libgnutls.a selbst
# stecken (Kodis Fehlerbild auf Darwin waren fehlende rpl_*-Definitionen).
syms=$("$NM" "$PREFIX/lib/libgnutls.a" 2>/dev/null)
for f in $(echo "$syms" | sed -n 's/^ *U \(rpl_[a-z_]*\)$/\1/p' | sort -u); do
  echo "$syms" | grep -q " T $f\$" || { echo "FEHLER: $f fehlt in libgnutls.a"; exit 1; }
done
done_stamp gnutls
