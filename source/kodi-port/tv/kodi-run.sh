#!/bin/sh
# Hülle um einen Kodi-Lauf (von kodi.sh start per setsid gestartet, Umgebung kommt von dort).
# - schreibt die eigene PID nach /tmp/flexkodi.d/pid (und löscht sie am Ende nur, wenn sie noch
#   die eigene ist),
# - schreibt, solange Kodi läuft, alle 2 s das Lebenszeichen flexkodi/running für den Launcher:
#   "<jetzt> <letzte Taste> <bereit> <minimiert>" (Sekunden; letzte Taste, bereit (1 = Fenster da und
#   Fernbedienung übernommen) und minimiert (1 = nach Home ausgeblendet, die Fernbedienung hat der
#   Launcher, 18.11.2026) liefert Kodi selbst über FLEX_KODI_STATE_DIR). flex.js schaltet damit
#   den Bildschirmschoner ab, behandelt Kodi wie eine App und hält den Auto-Standby des TVs an,
#   solange in Kodi Tasten gedrückt werden. Hängt nur am Kodi-Lauf, nicht am Wächter.
#   /var/local ist beim Booten für alle beschreibbar (/etc/profile: chmod -R 777), und der Eintrag
#   flexkodi kann getauscht werden: darum nach cd und Prüfung der Inode relativ im geprüften Ordner,
#   über eine neue Datei mit Zufallsnamen aus /dev/urandom und mv (rename ersetzt einen Symlink,
#   statt ihm zu folgen; ein Symlink running wird vorher entfernt, ein Ordner running ausgelassen,
#   weil mv ihn beträte),
# - räumt nach Kodis Ende die Brücke zu flex.js auf (Laufschlüssel, Brückendateien in
#   /tmp/flexkodi-rt/b; nur einzelne Einträge, docs/kodi/TVPLAYER-DESIGN.md 4.13),
# - liegen die Kodi-Daten in /APPS (kein USB-Stick), beendet es Kodi, sobald dort weniger als
#   APPS_STOP_KB frei sind (/APPS trägt auch den Launcher und den Boot-Haken),
# - protokolliert den Exit-Status (139 = SIGSEGV, 137 = von außen beendet, 141 = SIGPIPE).
# Aufruf: kodi-run.sh <kodi-programm> [argumente...]
R=/tmp/flexkodi.d
D=/var/local/applications/UI/flexkodi
RT=/tmp/flexkodi-rt
BRIDGE_FILES="play.json ctl player.json tvkeys tvack key keyok"
APPS_STOP_KB=80000
echo $$ > "$R/pid"
rm -f "$R/ready" "$R/lastkey" "$R/minimized" "$R/restore"

kodi_pid() {
  for d in /proc/[0-9]*; do
    [ "$(cat $d/comm 2>/dev/null)" = kodi-directfb ] && { echo "${d#/proc/}"; return 0; }
  done
  return 1
}

# Inode von $D, wenn er ein echter Ordner des Launchers (10009) ist; sonst leer
dino() {
  [ -d "$D" ] && [ ! -L "$D" ] || return 1
  ( cd "$D" 2>/dev/null || exit 1; set -- $(ls -lidn . 2>/dev/null); [ "$4" = 10009 ] && echo "$1" )
}

# cd nach $D und prüfen, dass "." derselbe Ordner ist (Inode DINO, Besitzer 10009); danach alles
# relativ zu ".". Nur in einer Subshell aufrufen.
ind() {
  [ -n "$DINO" ] && cd "$D" 2>/dev/null || return 1
  set -- $(ls -lidn . 2>/dev/null)
  [ "$1 $4" = "$DINO 10009" ]
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

# Elternordner und Brückenordner sind die eigenen (root 0755, darin 10009 0700, keine Symlinks)
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

DINO=$(dino)

beat() {
  n=0
  while :; do
    # flexkodi/ kann nach dem Start von Kodi noch entstehen (wie bisher: dann ab dort)
    [ -n "$DINO" ] || DINO=$(dino)
    if [ -n "$DINO" ]; then
      k=0; r=0; m=0
      [ -f "$R/lastkey" ] && read -r k < "$R/lastkey"
      [ -f "$R/ready" ] && r=1
      [ -f "$R/minimized" ] && m=1
      case "$k" in ''|*[!0-9]*) k=0 ;; esac
      # Zufallsname je Takt: unter einem vorhersagbaren Namen könnte ein Symlink auf ein Gerät
      # warten, dem "set -C" folgen würde (ash nimmt O_EXCL nur, wenn der Name noch fehlt)
      t=$(rndhex 8)
      [ -n "$t" ] && ( ind || exit 1
        [ -L running ] && rm -f running
        [ -d running ] && exit 1
        t=.hb.$t
        { [ -e "$t" ] || [ -L "$t" ]; } && exit 1
        (set -C; echo "$(date +%s) $k $r $m" > "$t") 2>/dev/null && { mv -f "$t" running 2>/dev/null || rm -f "$t"; } )
    fi
    n=$((n + 1))
    case "$HOME" in
      /APPS/*)
        if [ $((n % 8)) -eq 0 ]; then
          free=$(df -k /APPS | sed -n '2p' | sed 's/  */ /g' | cut -d' ' -f4)
          if [ "${free:-0}" -lt "$APPS_STOP_KB" ]; then
            echo "$(date) /APPS nur noch ${free:-?} KB frei, Kodi wird beendet"
            p=$(kodi_pid) && kill -TERM "$p"
          fi
        fi ;;
    esac
    sleep 2
  done
}

beat &
b=$!
"$@"
rc=$?
kill "$b" 2>/dev/null
# ein schon begonnenes mv des Lebenszeichens kann noch nachlaufen
sleep 1
[ -n "$DINO" ] || DINO=$(dino)
( ind && rm -f running )
rm -f "$R/ready" "$R/lastkey" "$R/key" "$R/minimized" "$R/restore"
# Brücke zu flex.js: Laufschlüssel und Dateien dieses Laufs entfernen (auch Reste .kp.*, falls Kodi
# zwischen Anlegen und Umbenennen starb), nur in den eigenen Ordnern
rtdirs_ok && bridge_clean
echo "Exit-Status: $rc"
[ "$(cat "$R/pid" 2>/dev/null)" = "$$" ] && rm -f "$R/pid"
