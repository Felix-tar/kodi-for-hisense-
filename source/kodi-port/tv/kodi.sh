#!/bin/sh
# FLEX-Kodi am TV: kodi.sh start | stop | status [zeilen] | running | close [--force] [netflix] [youtube]
# Programm in /APPS/kodi, Benutzerdaten (special://home) auf dem USB-Stick unter flexkodi/
# (ohne Stick in /APPS/kodi-home, aber nur, solange dort genug Platz ist: /APPS trägt auch den
# Launcher und unseren Boot-Haken). Kodi läuft als weiterer DirectFB-Client über dem Launcher und
# nimmt die Fernbedienung exklusiv (EVIOCGRAB), solange es läuft; beim Beenden oder Absturz gibt
# der Kernel sie automatisch an den TV zurück. Power beendet Kodi; Power 5 s halten beendet es
# sofort (auch wenn es hängt). Gestartet wird es über die Kodi-Kachel des Launchers
# (kodi-watch.sh) oder von Hand. Laufzeitdateien in /tmp/flexkodi.d (nur root).
# Brücke zu flex.js (TVPlayer, Lautstärke über tvkeys; docs/kodi/TVPLAYER-DESIGN.md 3.1, 3.2):
# Brückenordner im RAM /tmp/flexkodi-rt/b, angelegt und bewacht vom Wächter. start() erzeugt je
# Kodi-Lauf einen Laufschlüssel (16 Zufallsbytes) für Kodi root-only und für flex.js kurz als rt/key;
# Kodi bekommt nur den Pfad, der Schlüssel erscheint in keiner Umgebung und keinem Log.
K=/APPS/kodi
# Kodi benennt das Programm bei nur einer Plattform nach ihr (ENABLE_APP_AUTONAME)
BIN=$K/lib/kodi/kodi-directfb
R=/tmp/flexkodi.d
PIDF=$R/pid
RUNLOG=$R/run.log
# mindestens so viel muss in /APPS frei bleiben, wenn die Daten dort liegen (KB)
APPS_FLOOR_KB=150000
RT=/tmp/flexkodi-rt
BRIDGE_FILES="play.json ctl player.json tvkeys tvack key keyok"

# Bilder-Zwischenspeicher (18.11.2026): userdata/Thumbnails ist ein Link auf die Partition /OAD
# (Arbeitsbereich für Firmware-Updates, 950 MB frei; Updates sind gesperrt). Poster, Hintergründe und
# Schauspielerfotos füllten sonst /APPS bis unter APPS_FLOOR_KB. Das Ziel wird bei jedem Start angelegt:
# wird /OAD neu formatiert, lädt Kodi die Bilder neu; ohne /OAD bleiben sie ungespeichert.
THUMBS_OAD=/OAD/flexkodi/Thumbnails
thumbs_dir() {
  # nur wenn der Link dorthin zeigt (BusyBox des TVs hat kein readlink: ls -l)
  t="$1/.kodi/userdata/Thumbnails"
  [ -L "$t" ] || return 0
  case "$(ls -ld "$t" 2>/dev/null)" in *"-> $THUMBS_OAD") ;; *) return 0 ;; esac
  grep -q ' /OAD ' /proc/mounts || { echo "Hinweis: /OAD fehlt, Bilder werden nicht gespeichert"; return 0; }
  mkdir -p "$THUMBS_OAD" 2>/dev/null
  return 0
}

rundir() {
  mkdir -m 700 "$R" 2>/dev/null
  # muss ein echtes Verzeichnis von root mit 0700 sein (in /tmp könnte es sonst jemand anlegen)
  [ "$(ls -ldn "$R" 2>/dev/null | awk '{print $1, $3}')" = "drwx------ 0" ] || {
    echo "FEHLER: $R ist kein root-eigenes 0700-Verzeichnis"; return 1; }
}

# Läuft Kodi? Billig genug für Abfragen im Sekundentakt: die Hülle kodi-run.sh aus der PID-Datei.
running() {
  p=$(cat "$PIDF" 2>/dev/null)
  case "$p" in ''|*[!0-9]*) return 1 ;; esac
  [ -d "/proc/$p" ] && grep -q 'kodi-run.sh' "/proc/$p/cmdline" 2>/dev/null
}

# PID des Kodi-Prozesses selbst (Kind der Hülle; zum Beenden und für den Status)
pid_of_kodi() {
  for d in /proc/[0-9]*; do
    if [ "$(cat $d/comm 2>/dev/null)" = kodi-directfb ]; then echo "${d#/proc/}"; return 0; fi
  done
  return 1
}

data_dir() {
  # VIDAAs MountService hängt den Stick unter /tmp/lo_00X ein (Nummer kann wechseln; FAT, NTFS
  # oder exFAT). Nur ein beschreibbarer Stick zählt.
  for m in $(sed -n 's/^[^ ]* \(\/tmp\/lo_[0-9]*\) [^ ]* rw[, ].*/\1/p' /proc/mounts); do
    if [ -d "$m" ] && [ -w "$m" ]; then echo "$m/flexkodi"; return 0; fi
  done
  echo /APPS/kodi-home
}

dfbargs() {
  # wie beim Launcher: Optionen aus /mslib/directfbrc ("name = wert" -> "name=wert"), dazu MStars
  # libdfbinfo.so für die Hardware-Einstellungen (Grafikspeicher, GOP) aus /config/dfbrc.ini
  a=""
  while read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    # Diagnose: DirectFB-Fehlermeldungen nicht unterdrücken
    [ "$line" = quiet ] && [ "${FLEX_KODI_DIAG:-0}" = 1 ] && continue
    line=$(echo "$line" | sed 's/ *= */=/')
    a="${a:+$a,}$line"
  done < /mslib/directfbrc
  echo "$a,dfbinfo-dir=/mslib/libdfbinfo.so"
}

# $1 Zufallsbytes (höchstens 16) als Hex aus /dev/urandom; BusyBox hat kein od/tr, also hexdump,
# sonst md5sum wie mods/startscreen/install.sh
rndhex() {
  h=$(hexdump -v -n "$1" -e '1/1 "%02x"' /dev/urandom 2>/dev/null)
  case "$h" in *[!0-9a-f]*) h='' ;; esac
  [ ${#h} -eq $(($1 * 2)) ] || h=$(head -c 32 /dev/urandom | md5sum | cut -c1-$(($1 * 2)))
  case "$h" in *[!0-9a-f]*) h='' ;; esac
  [ ${#h} -eq $(($1 * 2)) ] && echo "$h"
}

# Elternordner und Brückenordner sind die vom Wächter angelegten (root 0755, darin 10009 0700,
# keine Symlinks); sonst wird dort nichts angefasst (D20: /tmp ist für alle beschreibbar)
rtdirs_ok() {
  [ ! -L "$RT" ] && [ ! -L "$RT/b" ] || return 1
  set -- $(ls -ldn "$RT" 2>/dev/null); [ "$1 $3" = "drwxr-xr-x 0" ] || return 1
  set -- $(ls -ldn "$RT/b" 2>/dev/null); [ "$1 $3 $4" = "drwx------ 10009 10009" ]
}

# einzelne bekannte Einträge + Reste .kp.* (nur normale Dateien/Symlinks), nie rekursiv (D20)
bridge_clean() {
  for f in $BRIDGE_FILES; do rm -f "$RT/b/$f" 2>/dev/null; done
  for f in "$RT/b"/.kp.*; do
    if [ -f "$f" ] || [ -L "$f" ]; then rm -f "$f"; fi
  done
}

# Laufschlüssel $1 für flex.js als $RT/b/key: root-eigen 0444 im 0700-Ordner von 10009, also lesbar
# nur für 10009 und root. Der Ordner gehört 10009, darum nur auf einen freien Namen und ohne einem
# Eintrag dort zu folgen: harter Link auf eine Datei im root-Ordner (link() schlägt fehl, wenn der
# Name belegt ist, auch durch einen Symlink; -n betritt keinen Symlink auf einen Ordner). Nicht mit
# "set -C; echo >": ash öffnet dabei einen Symlink auf ein Gerät oder eine FIFO ohne O_EXCL.
flexkey() {
  rm -f "$R/key.fx"
  (set -C; umask 222; echo "$1" > "$R/key.fx") 2>/dev/null || return 1
  ok=0
  [ -e "$RT/b/key" ] || [ -L "$RT/b/key" ] || { ln -n "$R/key.fx" "$RT/b/key" 2>/dev/null && ok=1; }
  rm -f "$R/key.fx"
  [ "$ok" = 1 ] && [ ! -L "$RT/b/key" ] || return 1
  set -- $(ls -ldn "$RT/b/key" 2>/dev/null)
  [ "$1 $2 $3" = "-r--r--r-- 1 0" ]
}

# Brücke für diesen Kodi-Lauf (D19): Reste eines früheren Laufs entfernen, Laufschlüssel für Kodi
# nach $R/key (root 0600 im root-0700-Ordner), für flex.js nach $RT/b/key (der Wächter löscht ihn
# nach der Quittung, spätestens 10 s nach "bereit"). Ist die Brücke aus oder schlägt etwas fehl,
# bekommt Kodi keine Brückenvariablen und spielt selbst (noBridge).
bridge() {
  if [ -e "$R/bridge-off" ]; then
    echo "Brücke zu flex.js für diesen Boot aus (Grund in $R/watch.log), Kodi spielt selbst"
    return 1
  fi
  rtdirs_ok || { echo "Brückenordner $RT/b fehlt oder stimmt nicht (läuft kodi-watch.sh?), Kodi spielt selbst"; return 1; }
  bridge_clean
  rm -f "$R/key"
  KEY=$(rndhex 16)
  if [ ${#KEY} -eq 32 ] && (set -C; umask 077; echo "$KEY" > "$R/key") 2>/dev/null && flexkey "$KEY"; then
    unset KEY
    export FLEX_KODI_BRIDGE_KEYFILE=$R/key FLEX_KODI_BRIDGE_DIR=$RT/b FLEX_KODI_TVKEYS=1
    return 0
  fi
  unset KEY
  rm -f "$R/key" "$RT/b/key" 2>/dev/null
  echo "Laufschlüssel nicht anlegbar, Kodi spielt selbst"
  return 1
}

# Hintergrund-Apps vor dem Kodi-Start beenden (Wunsch des Besitzers 05.11.2026: bei jedem Kodi-Start
# YouTube und Netflix schließen; RAM, Verbindungen, TV-Player). Automatisch nur beim Start über die
# Kodi-Kachel (der Wächter setzt FLEX_KODI_FROM_TILE=1): nur dann hat flex.js geprüft, dass keine App
# vorne liegt (homeIdle, noAppInStack). Der App-Manager allein erkennt ein vorne laufendes YouTube
# nicht. Von Hand: FLEX_KODI_AUTOCLOSE=1 kodi.sh start bzw. kodi.sh close [--force]. flex_app_close
# beendet sich spätestens nach 12 s selbst (flex.js wartet ab der Anforderung 25 s auf Kodi).
# Aus: Datei /APPS/kodi-autoclose.off (außerhalb /APPS/kodi, install.sh verschiebt den Ordner) oder
# FLEX_KODI_AUTOCLOSE=0. Ergebnis in $R/close.log und als Zeile "Apps:" in run.log.
CLOSE_OUT=""
close_apps() {
  h=$K/lib/kodi/flex_app_close
  if [ -e /APPS/kodi-autoclose.off ] || [ "${FLEX_KODI_AUTOCLOSE:-}" = 0 ]; then
    CLOSE_OUT="result off abgeschaltet"
  elif [ ! -x "$h" ]; then
    CLOSE_OUT="result off Helfer fehlt"
  else
    CLOSE_OUT=$(LD_LIBRARY_PATH=/mslib:/mslib/utopia:/lib "$h" "$@" 2>&1)
  fi
  { echo "--- $(date '+%F %T') close $*"; echo "$CLOSE_OUT"; } >> "$R/close.log"
  tail -n 200 "$R/close.log" > "$R/close.log.t" && mv "$R/close.log.t" "$R/close.log"
  echo "$CLOSE_OUT"
}

start() {
  rundir || return 1
  # nur ein Start gleichzeitig (Kachel und Hand); eine Sperre älter als 1 min ist ein Rest
  find "$R" -maxdepth 1 -name lock -type d -mmin +1 -exec rmdir {} \; 2>/dev/null
  mkdir "$R/lock" 2>/dev/null || { echo "Kodi startet gerade schon"; return 0; }
  trap 'rmdir "$R/lock" 2>/dev/null' EXIT
  if running || pid_of_kodi > /dev/null; then
    # nach Home ausgeblendet (18.11.2026): die Kodi-Kachel holt es zurück (Kodi prüft "restore"
    # alle 0,3 s, EvdevInputHandler::CheckRestore)
    if [ -f "$R/minimized" ]; then : > "$R/restore"; echo "Kodi zurückgeholt"; return 0; fi
    echo "Kodi läuft schon"; return 0
  fi
  [ -x "$BIN" ] || { echo "FEHLER: $BIN fehlt"; return 1; }
  HOME=$(data_dir)
  if [ "$HOME" = /APPS/kodi-home ]; then
    free=$(df -k /APPS | sed -n '2p' | sed 's/  */ /g' | cut -d' ' -f4)
    if [ "${free:-0}" -lt "$APPS_FLOOR_KB" ]; then
      echo "FEHLER: kein beschreibbarer USB-Stick und in /APPS nur ${free:-?} KB frei, Kodi startet nicht"
      return 1
    fi
  fi
  mkdir -p "$HOME" || { echo "FEHLER: $HOME nicht anlegbar"; return 1; }
  thumbs_dir "$HOME"
  # vor allen Exporten (der Helfer braucht nur die Bibliotheken des TVs) und vor LD_PRELOAD/setsid
  if [ "${FLEX_KODI_FROM_TILE:-0}" = 1 ]; then
    close_apps --bg-confirmed > /dev/null
  elif [ "${FLEX_KODI_AUTOCLOSE:-}" = 1 ]; then
    close_apps > /dev/null
  else
    CLOSE_OUT="result off Start nicht über die Kachel"
  fi
  export HOME
  export KODI_HOME=$K/share/kodi KODI_BIN_HOME=$K/lib/kodi
  export DIRECTFB_SESSION=0
  export LD_LIBRARY_PATH=$K/lib:/lib:/mslib:/mslib/utopia:/std_lib/lib:/tvservice/conflictlib:/system/lib:/usr/local/lib
  DFBARGS=$(dfbargs)
  export DFBARGS
  export FLEX_KODI_INPUT=/dev/input/event0 FLEX_KODI_GRAB=1
  # Kodi meldet dort "bereit" und die Zeit der letzten Taste (kodi-run.sh gibt es weiter)
  export FLEX_KODI_STATE_DIR=$R
  # eigene alsa-lib (1.2.x) mit eigener Konfiguration, nicht die 1.0.23-Dateien des TVs
  export ALSA_CONFIG_DIR=$K/share/alsa ALSA_CONFIG_PATH=$K/share/alsa/alsa.conf
  # CA-Zertifikate für HTTPS (der TV hat keine für OpenSSL)
  export SSL_CERT_FILE=$K/share/kodi/system/certs/cacert.pem
  # Statusseite des AV-Receivers (Denon/Marantz, nur lesen) für Kodis Lautstärkebalken über ARC
  # (FlexVolumeBar.h), z. B. http://<ip>:8080/goform/formMainZone_MainZoneXmlStatusLite.xml
  if [ -s "$K/avr.url" ]; then
    FLEX_KODI_AVR_URL=$(awk 'NR == 1 { sub(/\r$/, ""); print; exit }' "$K/avr.url")
    export FLEX_KODI_AVR_URL
  fi
  # OpenSSL 1.1.1 prüft beim Start per SIGILL, ob der CPU-Zähler (CNTVCT, "ARMV7_TICK") lesbar ist,
  # aber nur auf dem Kern, auf dem es gerade läuft. Der TV-Kernel erlaubt das nicht auf jedem Kern:
  # Kodi starb sonst mit "Illegal instruction" in _armv7_tick (Zufallsgenerator, 29.10.2026). Mit
  # OPENSSL_armcap prüft OpenSSL nichts selbst; der Wert kommt aus den Features, die der Kernel für
  # alle Kerne meldet (NEON 1, AES 4, SHA1 8, SHA256 16, PMULL 32), ohne den Zähler (2).
  OPENSSL_armcap=$(awk '/^Features/ { v = 0; for (i = 3; i <= NF; i++) {
      if ($i == "neon") v += 1; else if ($i == "aes") v += 4; else if ($i == "sha1") v += 8
      else if ($i == "sha2") v += 16; else if ($i == "pmull") v += 32 }
      if (v % 2 == 0) v = 0
      print v; exit }' /proc/cpuinfo)
  export OPENSSL_armcap=${OPENSSL_armcap:-0}
  set -- --windowing=directfb --audio-backend=alsa
  # ausführliches Log nur auf Wunsch (es wächst schnell)
  [ "${FLEX_KODI_DEBUG:-0}" = 1 ] || [ "${FLEX_KODI_DIAG:-0}" = 1 ] && set -- "$@" --debug
  cd /tmp || return 1
  rm -f "$RUNLOG"
  echo "Start: HOME=$HOME" > "$RUNLOG"
  echo "Apps: $(echo "$CLOSE_OUT" | tail -n 1)" >> "$RUNLOG"
  # Brücke zu flex.js (vor LD_PRELOAD, damit die Hilfsprogramme ohne libSegFault laufen). Spulen
  # über den TV-Player (setMpctrlFlag) nur mit FLEX_KODI_TRICKPLAY=1 von Hand (D21 am TV nicht
  # bestätigt, P2); ohne springt Kodi ±30 s je Druck.
  bridge
  # Absturzbericht immer an: glibcs libSegFault schreibt bei einem Absturz Register und
  # Backtrace in die Ausgabe (Adressen mit kodi-port/tests/addr2line.sh gegen das ungestrippte
  # kodi-directfb der VM auflösen). Eigene Sitzung (setsid), damit Kodi das Ende der SSH-Sitzung
  # bzw. des Wächters übersteht (BusyBox hat kein nohup).
  export LD_PRELOAD=/lib/libSegFault.so SEGFAULT_SIGNALS="segv bus ill fpe abrt"
  setsid "$K/kodi-run.sh" "$BIN" "$@" >> "$RUNLOG" 2>&1 < /dev/null &
  # warten, bis die Hülle ihre PID eingetragen hat (sonst meldete running() kurz "läuft nicht")
  i=0
  while ! running && [ $i -lt 20 ]; do sleep 0.1 2>/dev/null || sleep 1; i=$((i + 1)); done
  echo "Kodi gestartet, Daten: $HOME, Ausgabe: $RUNLOG"
}

stop() {
  p=$(pid_of_kodi) || { echo "Kodi läuft nicht"; return 0; }
  kill -TERM "$p"
  i=0
  while [ -d "/proc/$p" ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
  if [ -d "/proc/$p" ]; then echo "Kodi reagiert nicht, KILL"; kill -KILL "$p"; fi
  echo "Kodi beendet"
}

status() {
  if p=$(pid_of_kodi); then
    echo "Kodi läuft (PID $p), $(grep -E 'VmRSS' /proc/$p/status)"
  else
    echo "Kodi läuft nicht"
  fi
  [ -e "$R/bridge-off" ] && echo "Brücke zu flex.js: für diesen Boot aus (Grund in $R/watch.log)"
  l="$(sed -n 's/^Start: HOME=//p' "$RUNLOG" 2>/dev/null)/.kodi/temp/kodi.log"
  [ -f "$l" ] && { echo "--- $l (Ende)"; tail -n "${1:-30}" "$l"; }
  [ -f "$RUNLOG" ] && { echo "--- $RUNLOG (Ende)"; tail -n 15 "$RUNLOG"; }
}

case "$1" in
  start) start ;;
  stop) stop ;;
  status) status "$2" ;;
  running) running ;;
  # von Hand: kodi.sh close [--force] [netflix] [youtube]; ohne --force nur Probe (--dry). --force
  # nur, wenn die Startseite vorne ist (sonst könnte ein vorne laufendes YouTube beendet werden).
  close)
    shift
    if [ "$1" = --force ]; then shift; set -- --bg-confirmed "$@"; else set -- --dry "$@"; fi
    rundir && close_apps "$@" ;;
  *) echo "Aufruf: $0 start|stop|status [zeilen]|running|close [--force] [netflix] [youtube]"; exit 2 ;;
esac
