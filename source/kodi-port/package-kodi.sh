#!/bin/bash
# Kodi-Paket für den TV: installiert den Build nach ~/flex/stage-kodi/APPS/kodi, entfernt, was am
# TV nichts nützt (Desktop-Dateien, Touch-Skin, Header), strippt die Programme, legt die
# ALSA-Konfiguration und das Startskript dazu und packt alles nach ~/flex/kodi-tv.tar.gz.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
B=${FLEX_BUILD_ROOT:-$HOME/flex}/build-kodi
ST=${FLEX_BUILD_ROOT:-$HOME/flex}/stage-kodi
OUT=${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-tv.tar.gz
rm -rf "$ST"
DESTDIR="$ST" ninja -C "$B" install > "$LOGS/kodi-install.log"
K=$ST/APPS/kodi
BIN=$K/lib/kodi/kodi-directfb
[ -x "$BIN" ] || { echo "FEHLER: $BIN fehlt"; exit 1; }
# Keine Bibliothek darf mit einem Pfad der VM eingetragen sein (libMali/libEGL ohne SONAME
# würden sonst mit ~/flex/sysroot-kodi/lib/... in NEEDED landen)
needed=$("/usr/bin/$HOST-readelf" -d "$BIN" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
echo "NEEDED: $(echo $needed)"
case "$needed" in */*) echo "FEHLER: NEEDED enthält einen Pfad"; exit 1 ;; esac
# Desktop-Linux-Beigaben, der Touch-Skin und die Versionsprüfung (ein Python-Add-on; dieses Kodi
# hat kein Python)
rm -rf "$K/include" "$K/share/applications" "$K/share/icons" "$K/share/metainfo" \
  "$K/share/xsessions" "$K/share/wayland-sessions" "$K/lib/firewalld" "$K/share/kodi/addons/skin.estouchy" \
  "$K/share/kodi/addons/service.xbmc.versioncheck"
# Der TV hat keine CA-Zertifikate für OpenSSL: Mozillas Sammlung, wie curl sie verteilt (SHA256 von
# curl.se geprüft), kodi.sh setzt SSL_CERT_FILE darauf (sonst scheitert HTTPS, z. B. Kodis
# Add-on-Repository mit den Sprachpaketen)
CA=$DL/cacert.pem
curl -fsSL -o "$CA.new" https://curl.se/ca/cacert.pem
want=$(curl -fsSL https://curl.se/ca/cacert.pem.sha256 | cut -d' ' -f1)
[ "$(sha256sum "$CA.new" | cut -d' ' -f1)" = "$want" ] || { echo "FEHLER: cacert.pem SHA256"; exit 1; }
mv "$CA.new" "$CA"
mkdir -p "$K/share/kodi/system/certs"
install -m 644 "$CA" "$K/share/kodi/system/certs/cacert.pem"
find "$K" -type f \( -name 'kodi-directfb' -o -name '*.so' -o -name '*.so.*' \) -exec "$STRIP" --strip-unneeded {} \;
mkdir -p "$K/share/alsa"
cp -r "$PREFIX/share/alsa-tv/." "$K/share/alsa/"
# Deliver ALSA runtime configuration without generated host build files.
find "$K/share/alsa" -type f \( -name 'Makefile*' -o -name '*.am' \) -delete
# Helfer für den ES-Weg (echte Bildrate): eigener Prozess, nur er lädt die Herstellerbibliotheken
"$CC" -O2 -Wall -I"$HERE/kodi-tree/xbmc/cores/TVPlayer" -o "$K/lib/kodi/flex_es_helper.bin" \
  "$F/native/flex_es_helper.c" -ldl -lpthread "-Wl,-T,$LDSCRIPT"
"$STRIP" --strip-unneeded "$K/lib/kodi/flex_es_helper.bin"
chmod 755 "$K/lib/kodi/flex_es_helper.bin"
install -m 755 "$HERE/tv/flex_es_helper" "$K/lib/kodi/flex_es_helper"
# Helfer, der vor jedem Kodi-Start pausiertes Netflix/YouTube beendet (kodi.sh close_apps)
"$CC" -O2 -Wall -o "$K/lib/kodi/flex_app_close" "$F/native/flex_app_close.c" -ldl "-Wl,-T,$LDSCRIPT"
"$STRIP" --strip-unneeded "$K/lib/kodi/flex_app_close"
chmod 755 "$K/lib/kodi/flex_app_close"
install -m 755 "$HERE/tv/kodi.sh" "$K/kodi.sh"
for f in kodi-run.sh kodi-watch.sh; do
  install -m 755 "$HERE/tv/$f" "$K/$f"
done
# Optional German language package. Local source is the verified official Omega add-on.
if [ -n "${FLEX_KODI_GERMAN_PACK:-}" ]; then
  expected=aee272346f7c91e7d28e3b0e93c48651ae7bbc0d9740173595c799f022a858c5
  printf '%s  %s\n' "$expected" "$FLEX_KODI_GERMAN_PACK" | sha256sum -c -
  python3 -m zipfile -e "$FLEX_KODI_GERMAN_PACK" "$K/share/kodi/addons"
fi
# Größen fürs Protokoll (Platz in /APPS ist knapp)
du -sk "$K" "$BIN" "$K/share/kodi" "$K/share/kodi/addons" 2>/dev/null
du -sk "$K"/share/kodi/addons/* 2>/dev/null | sort -n | tail -8
# Besitzer root im Archiv: am TV läuft alles als root, und uid 1000 ist dort der Systemdienst
# SystemService (sonst gehörten ihm die Skripte, die root ausführt)
tar -C "$ST/APPS" --owner=0 --group=0 --numeric-owner -czf "$OUT" kodi
ls -l "$OUT"
sha256sum "$OUT"
# BusyBox am TV hat nur md5sum
md5sum "$OUT"
