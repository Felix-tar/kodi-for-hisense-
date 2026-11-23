#!/bin/sh
# Kodi-Paket am TV installieren: install.sh <paket.tar.gz> <md5>
# Entpackt nach /APPS/kodi. Eine vorhandene Fassung wird vorher zu /APPS/kodi.prev (wenn der Platz
# reicht, sonst nach Rückfrage gelöscht: dann mit FLEX_KODI_REPLACE=1 erneut aufrufen). Kodi muss
# dafür beendet sein. Die Benutzerdaten (USB-Stick bzw. /APPS/kodi-home) bleiben unberührt.
P=$1
SUM=$2
[ -f "$P" ] || { echo "FEHLER: Paket $P fehlt"; exit 1; }
have=$(md5sum "$P" | cut -d' ' -f1)
[ "$have" = "$SUM" ] || { echo "FEHLER: MD5 $have, erwartet $SUM"; exit 1; }
if [ -x /APPS/kodi/kodi.sh ] && /APPS/kodi/kodi.sh status | grep -q 'läuft (PID'; then
  echo "FEHLER: Kodi läuft, zuerst /APPS/kodi/kodi.sh stop"; exit 1
fi
# Platzbedarf: entpackte Größe ~ 2,6 x Paket (grob, ohne Listing)
need=$(( $(du -k "$P" | cut -f1) * 26 / 10 ))
free=$(df -k /APPS | sed -n '2p' | sed 's/  */ /g' | cut -d' ' -f4)
echo "Paket $(du -k "$P" | cut -f1) KB, geschätzt $need KB entpackt, frei in /APPS: $free KB"
if [ -d /APPS/kodi ]; then
  old=$(du -sk /APPS/kodi | cut -f1)
  if [ $((free)) -gt $((need + 20000)) ]; then
    rm -rf /APPS/kodi.prev && mv /APPS/kodi /APPS/kodi.prev && echo "alte Fassung -> /APPS/kodi.prev"
  elif [ "${FLEX_KODI_REPLACE:-0}" = 1 ]; then
    rm -rf /APPS/kodi.prev /APPS/kodi && echo "alte Fassung gelöscht (FLEX_KODI_REPLACE=1)"
  else
    echo "FEHLER: zu wenig Platz für alte + neue Fassung (alt $old KB). FLEX_KODI_REPLACE=1 ersetzt ohne Sicherung."
    exit 1
  fi
else
  [ $((free)) -gt $((need + 20000)) ] || { echo "FEHLER: zu wenig Platz in /APPS"; exit 1; }
fi
tar -xzf "$P" -C /APPS || { echo "FEHLER: Entpacken fehlgeschlagen"; exit 1; }
# root führt die Skripte aus: nichts darf einem anderen Benutzer gehören (ältere Pakete trugen uid 1000,
# am TV der Systemdienst SystemService)
chown -R 0:0 /APPS/kodi
chmod 755 /APPS/kodi/kodi.sh /APPS/kodi/lib/kodi/kodi-directfb
[ -f /APPS/kodi/lib/kodi/flex_app_close ] && chmod 755 /APPS/kodi/lib/kodi/flex_app_close
left=$(df -k /APPS | sed -n '2p' | sed 's/  */ /g' | cut -d' ' -f4)
echo "installiert: $(du -sk /APPS/kodi | cut -f1) KB, frei: $left KB"
# kodi.sh startet ohne USB-Stick erst ab APPS_FLOOR_KB (150000) frei: die Sicherung kann das verhindern
if [ -d /APPS/kodi.prev ] && [ $((left)) -lt 150000 ]; then
  echo "WARNUNG: nur $left KB frei, Kodi startet ohne USB-Stick erst ab 150000 KB."
  echo "  Läuft die neue Fassung, /APPS/kodi.prev löschen (das alte Paket liegt auf der Build-VM)."
fi
