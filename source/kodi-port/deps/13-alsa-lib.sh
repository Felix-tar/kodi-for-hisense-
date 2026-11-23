#!/bin/bash
# alsa-lib (aktuell, statisch). Kodi 21.3 verlangt >= 1.0.27 (Kanal-Zuordnung/chmap); die
# libasound des TVs ist 1.0.23. Die neue Bibliothek spricht über die stabile Kernel-Schnittstelle
# mit dem ALSA-Treiber des TVs und liest ihre Konfiguration aus Kodis Ordner (KODI_TV), nicht
# aus /usr/share/alsa des TVs.
# Version und SHA-256 aus dem geprüften Referenz-Build.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done alsa-lib && exit 0
KODI_TV=/APPS/kodi
V=1.2.16.1
H=f740db7f488255944ffd4428416ee3390a96742856916433df468c281436480e
A=$DL/alsa-lib-$V.tar.bz2
[ -f "$A" ] || curl -fsSL -o "$A" "https://www.alsa-project.org/files/pub/lib/alsa-lib-$V.tar.bz2"
echo "$H  $A" | sha256sum -c - || { echo "FEHLER: SHA256 von alsa-lib $V passt nicht"; exit 1; }
rm -rf "$BUILD/alsa-lib" && mkdir -p "$BUILD/alsa-lib"
tar -xjf "$A" -C "$BUILD/alsa-lib" --strip-components=1
cd "$BUILD/alsa-lib"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static --disable-python \
  --disable-old-symbols --disable-topology --with-configdir=$KODI_TV/share/alsa \
  --with-plugindir=$KODI_TV/lib/alsa-lib
make -j"$JOBS"
# nur Bibliothek, Header und alsa.pc; die Konfiguration gehört zum TV-Paket (siehe unten), nicht
# nach $KODI_TV auf der VM
make -C src install-exec
make -C include install
make -C utils install-pkgconfigDATA
# Konfiguration zum Mitliefern auf den TV (landet in $KODI_TV/share/alsa)
rm -rf "$PREFIX/share/alsa-tv" && mkdir -p "$PREFIX/share/alsa-tv"
cp -r src/conf/* "$PREFIX/share/alsa-tv/" 2>/dev/null || true
echo "alsa-lib $V installiert"
done_stamp alsa-lib
