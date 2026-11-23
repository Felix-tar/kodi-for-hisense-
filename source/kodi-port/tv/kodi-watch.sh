#!/bin/sh
# Kodi tile supervisor. Start from the existing authorized service manager.
# hisenseUI = 10009) kann keine Programme starten; er legt nur Anforderungen an:
#   flexkodi/start  Inhalt = Uhrzeit in Sekunden. Nur eine frische Anforderung (höchstens 5 s
#                   alt, nur Ziffern, normale kleine Datei) startet /APPS/kodi/kodi.sh start;
#                   ältere (z. B. über einen Standby hinweg liegengeblieben) werden verworfen.
#                   Sonst wird nichts aus der Datei verwendet.
#   flexkodi/stop   beendet Kodi (kodi.sh stop), z. B. wenn eine App oder ein Systemdialog den
#                   Bildschirm braucht (flex.js entscheidet das).
# Das Lebenszeichen flexkodi/running schreibt kodi-run.sh, solange Kodi läuft.
# /var/local ist beim Booten für alle beschreibbar (/etc/profile: chmod -R 777): der Wächter setzt
# flexkodi/ bei jeder vollen Prüfung (siehe Takt) auf 10009:0755 (dann legen dort nur noch root und
# 10009 etwas an), liest dort nur kleine normale Dateien (find -type f, dd mit Längengrenze) und
# löscht nur einzelne Einträge mit rm -f (das einem Symlink nicht folgt); er legt dort nichts an
# außer dem Ordner selbst und dem Symlink rt. Der Eintrag flexkodi selbst liegt in einem 0777-Ordner
# und kann jederzeit getauscht werden: darum läuft alles in flexkodi/ nach cd und Prüfung der Inode
# relativ zu ".".
# Brücke zu flex.js (TVPlayer, Lautstärke; docs/kodi/TVPLAYER-DESIGN.md 3.1, 4.13): im RAM
#   /tmp/flexkodi-rt (root 0755) mit dem Brückenordner b (10009 0700), für flex.js erreichbar über
#   den Symlink flexkodi/rt. Bei jeder vollen Prüfung: Ordner und Symlink prüfen bzw. anlegen, im
#   Brückenordner untergeschobene Einträge (Symlink, FIFO, > 8 KB) einzeln entfernen und den
#   Laufschlüssel rt/key löschen, sobald flex.js ihn quittiert hat (keyok), spätestens 10 Runden nach
#   "bereit" und sobald Kodi nicht mehr läuft. "fail closed" (D20): nie rekursiv löschen, nie mv,
#   ln -s nur auf einen freien Namen; stimmt etwas nicht, wird nichts repariert, die Brücke bleibt für
#   diesen Boot aus (Merker /tmp/flexkodi.d/bridge-off, Grund in watch.log) und Kodi spielt selbst.
# Takt: eine Runde je Sekunde. Anforderungen (start, stop) und ein running, das keine normale Datei
#   ist, erkennt jede Runde ein eingebauter Test der Shell (kein Prozess). Die volle Prüfung (rtdir
#   und fkround, beide mit cd, Inode und chmod von flexkodi) läuft in jeder Runde mit Anforderung
#   (also immer vor kodi.sh start), in den ersten BOOTN Runden nach dem Start des Wächters
#   (/etc/profile macht flexkodi erst nach rcS 0777) und sonst jede IDLEN-te Runde; solange Kodi
#   läuft oder ein Laufschlüssel liegt, zusätzlich rtdir in jeder Runde. Ohne Kodi kostet eine
#   Runde damit meist nur sleep (wie v6).
D=/var/local/applications/UI/flexkodi
K=/APPS/kodi
R=/tmp/flexkodi.d
L=$R/watch.log
RT=/tmp/flexkodi-rt
BRIDGE_FILES="play.json ctl player.json tvkeys tvack key keyok"
KEYAGE=0
FKBAD=""
# ohne Kodi volle Prüfung nur jede IDLEN-te Runde, außer in den ersten BOOTN Runden (siehe Takt)
IDLEN=10
BOOTN=300
KPID=""
KRUN=""

# Inode von $D, wenn er ein echter Ordner des Launchers (10009) ist; sonst leer
dino() {
  [ -d "$D" ] && [ ! -L "$D" ] || return 1
  ( cd "$D" 2>/dev/null || exit 1; set -- $(ls -lidn . 2>/dev/null); [ "$4" = 10009 ] && echo "$1" )
}

# In $D arbeiten: cd und prüfen, dass "." noch derselbe Ordner wie beim Start ist (Inode DINO,
# Besitzer 10009); die Shell bleibt danach dort, alles Weitere relativ zu ".". Hält den Ordner auf
# 0755 (chmod nur bei Bedarf: jeder chmod schreibt die Inode auf den Flash). Nur in einer Subshell.
ind() {
  [ -n "$DINO" ] && cd "$D" 2>/dev/null || return 1
  set -- $(ls -lidn . 2>/dev/null)
  [ "$1 $4" = "$DINO 10009" ] || return 1
  [ "$2" = drwxr-xr-x ] || chmod 755 .
}

# Ist $1 ein Symlink genau auf $2? (BusyBox des TVs hat kein readlink.) Die Zeile von ls muss auf
# " $1 -> $2" enden, darf nur ein " -> " enthalten, und die Größe des Symlinks (Länge seines Ziels)
# muss stimmen: ein Ziel mit " -> " oder Zeilenumbruch fällt so durch.
islink_to() {
  [ -L "$1" ] || return 1
  lt=$2
  ll=$(ls -ldn "$1" 2>/dev/null)
  case "$ll" in
    *" -> "*" -> "*) return 1 ;;
    l*" $1 -> $lt") ;;
    *) return 1 ;;
  esac
  set -- $ll
  [ "$5" = "${#lt}" ]
}

# Brücke für diesen Boot aus ($1 = Grund, D20); nur unseren Symlink rt entfernen, nie ein Ziel
bridge_off() {
  [ -e "$R/bridge-off" ] || { : > "$R/bridge-off"; echo "$(date) Brücke aus: $1" >> "$L"; }
  ( ind && [ -L rt ] && rm -f rt )
}

# Elternordner und Brückenordner sind die eigenen (root 0755, darin 10009 0700, keine Symlinks)
rtdirs_ok() {
  [ ! -L "$RT" ] && [ ! -L "$RT/b" ] || return 1
  set -- $(ls -ldn "$RT" 2>/dev/null); [ "$1 $3" = "drwxr-xr-x 0" ] || return 1
  set -- $(ls -ldn "$RT/b" 2>/dev/null); [ "$1 $3 $4" = "drwx------ 10009 10009" ]
}

# Läuft Kodi (Hülle aus der PID-Datei lebt, wie kodi.sh running) oder startet es gerade (Sperre von
# kodi.sh start, höchstens 1 min alt)? Beim Start liegt der Laufschlüssel schon vor der PID-Datei.
kodi_alive() {
  p=""
  [ -f "$R/pid" ] && read -r p < "$R/pid"
  case "$p" in
    ''|*[!0-9]*) ;;
    *) [ -d "/proc/$p" ] && grep -q 'kodi-run.sh' "/proc/$p/cmdline" 2>/dev/null && return 0 ;;
  esac
  [ -d "$R/lock" ] && [ -z "$(find "$R" -maxdepth 1 -name lock -type d -mmin +1 2>/dev/null)" ]
}

# Braucht diese Runde die volle Prüfung, weil Kodi läuft oder ein Laufschlüssel liegt? Billig für
# den Sekundentakt: nur eingebaute Tests; grep auf die cmdline nur einmal je neuer PID (Ergebnis in
# KPID/KRUN). Während kodi.sh start liegt der Schlüssel schon vor der PID-Datei. Über das Löschen
# des Schlüssels entscheidet weiter kodi_alive (rtkey).
kodi_busy() {
  { [ -e "$RT/b/key" ] || [ -L "$RT/b/key" ]; } && return 0
  p=""
  [ -f "$R/pid" ] && read -r p < "$R/pid"
  case "$p" in ''|*[!0-9]*) return 1 ;; esac
  [ -d "/proc/$p" ] || return 1
  if [ "$p" != "$KPID" ]; then
    KPID=$p
    KRUN=""
    grep -q 'kodi-run.sh' "/proc/$p/cmdline" 2>/dev/null && KRUN=1
  fi
  [ -n "$KRUN" ]
}

# Liegt in $D eine Anforderung (start, stop) oder ein running, das keine normale Datei ist (Symlink,
# FIFO)? Nur eingebaute Tests über den Pfad: sie lesen und löschen nichts. Gelesen und gelöscht wird
# nur in fkround, nach cd und Inode-Prüfung.
fkpending() {
  [ -e "$D/start" ] || [ -L "$D/start" ] || [ -e "$D/stop" ] || [ -L "$D/stop" ] || [ -L "$D/running" ] ||
    { [ -e "$D/running" ] && [ ! -f "$D/running" ] && [ ! -d "$D/running" ]; }
}

# Laufschlüssel für flex.js (D19) höchstens bis zur Quittung keyok bzw. 10 Runden nach "bereit",
# nie über das Ende von Kodi hinaus (kodi.sh start legt ihn an, kodi-run.sh räumt am Ende auf)
rtkey() {
  if [ -e "$RT/b/key" ] || [ -L "$RT/b/key" ]; then
    if [ -e "$RT/b/keyok" ] || ! kodi_alive; then rm -f "$RT/b/key" 2>/dev/null
    elif [ -e "$R/ready" ]; then KEYAGE=$((KEYAGE + 1)); [ "$KEYAGE" -ge 10 ] && rm -f "$RT/b/key" 2>/dev/null
    fi
  else
    KEYAGE=0
  fi
}

# RAM-Brücke prüfen bzw. herstellen (vor der Schleife und bei jeder vollen Prüfung, siehe Takt).
# Rückgabe 1 = Brücke aus.
rtdir() {
  if [ -e "$R/bridge-off" ]; then
    # aus: nichts anlegen, nichts reparieren; einen liegengebliebenen Laufschlüssel aber weiter
    # befristen, solange beide Ordner noch die eigenen sind
    if [ -e "$RT/b/key" ] || [ -L "$RT/b/key" ]; then rtdirs_ok && rtkey; fi
    return 1
  fi
  # 1) /tmp mit Sticky-Bit (sonst könnte jeder $RT umbenennen); den Elternordner nur anlegen, nie
  #    verschieben (umask 077: kein Augenblick, in dem er für andere offen wäre)
  set -- $(ls -ldn /tmp 2>/dev/null)
  case "$1 $3" in d????????t" 0") ;; *) bridge_off "/tmp ohne Sticky-Bit"; return 1 ;; esac
  [ -e "$RT" ] || [ -L "$RT" ] || (umask 077; mkdir -m 755 "$RT") 2>/dev/null
  { [ ! -L "$RT" ] && set -- $(ls -ldn "$RT" 2>/dev/null) && [ "$1 $3" = "drwxr-xr-x 0" ]; } || { bridge_off "$RT falsch"; return 1; }
  # 2) Brückenordner: in $RT legt nur root an, also kein Rennen
  [ -e "$RT/b" ] || [ -L "$RT/b" ] || { (umask 077; mkdir -m 700 "$RT/b") && chown 10009:10009 "$RT/b"; } 2>/dev/null
  { [ ! -L "$RT/b" ] && set -- $(ls -ldn "$RT/b" 2>/dev/null) && [ "$1 $3 $4" = "drwx------ 10009 10009" ]; } || { bridge_off "$RT/b falsch"; return 1; }
  # 3) Symlink flexkodi/rt -> $RT/b (nach cd und Inode-Prüfung, relativ): nur einen Symlink oder
  #    eine Datei ersetzen, nie einen Ordner; ln -s nur auf den freien Namen (-n: auch einen Symlink
  #    auf einen Ordner nicht betreten, sonst legte ln den Link darin an), danach prüfen
  ( ind || exit 1
    islink_to rt "$RT/b" && exit 0
    { [ -L rt ] || [ -f rt ]; } && rm -f rt
    { [ -e rt ] || [ -L rt ]; } && exit 1
    ln -sn "$RT/b" rt 2>/dev/null && islink_to rt "$RT/b" || exit 1
    echo "$(date) Brücke: $D/rt -> $RT/b angelegt" >> "$L" ) || { bridge_off "$D getauscht oder rt nicht herstellbar"; return 1; }
  # 4) bekannte Namen, die ein Symlink oder keine normale Datei sind, einzeln entfernen (einen
  #    Ordner lässt rm -f stehen); zu große Dateien (> 8 KB): eine Suche je Runde, meist ohne Treffer
  for f in $BRIDGE_FILES; do
    if [ -L "$RT/b/$f" ] || { [ -e "$RT/b/$f" ] && [ ! -f "$RT/b/$f" ]; }; then rm -f "$RT/b/$f" 2>/dev/null; fi
  done
  if [ -n "$(find "$RT/b" -maxdepth 1 -type f -size +16 2>/dev/null)" ]; then
    for f in $BRIDGE_FILES; do
      [ -n "$(find "$RT/b/$f" -maxdepth 0 -type f -size +16 2>/dev/null)" ] && rm -f "$RT/b/$f"
    done
  fi
  # 5) Laufschlüssel befristen
  rtkey
}

# Anforderungen des Launchers abholen, nur im geprüften Ordner (ind). Ausgabe "stop" oder
# "start <inhalt>"; stop verwirft eine gleichzeitige Startanforderung (wie bisher). Nebenbei wird
# ein running, das keine normale Datei ist (Symlink, FIFO), entfernt: flex.js liest es jede Sekunde
# und hinge an einer FIFO (kodi-run.sh legt es immer als normale Datei an). Nur in $(...) aufrufen;
# Exit 1 = $D ist nicht mehr derselbe Ordner (dann nichts lesen, nichts löschen).
fkround() {   # $1 = start: nur die Startanforderung (nach einem Stopp)
  ind || exit 1
  if [ -L running ] || { [ -e running ] && [ ! -f running ] && [ ! -d running ]; }; then rm -f running; fi
  if [ "$1" != start ] && { [ -e stop ] || [ -L stop ]; }; then
    rm -f stop start
    echo stop
    exit 0
  fi
  if [ -e start ] || [ -L start ]; then
    t=""
    if [ -n "$(find . -maxdepth 1 -name start -type f -size -2 2>/dev/null)" ]; then
      t=$(dd if=start bs=16 count=1 2>/dev/null)
    fi
    rm -f start
    echo "start $t"
  fi
}

mkdir -m 700 "$R" 2>/dev/null
[ "$(ls -ldn "$R" 2>/dev/null | awk '{print $1, $3}')" = "drwx------ 0" ] || exit 1
[ -L "$D" ] && { echo "$(date) $D ist ein Symlink, Abbruch" >> "$L"; exit 1; }
[ -d "${D%/*}" ] || exit 1
mkdir -p "$D"
# dem Launcher übergeben (chown -h folgt keinem Symlink); /etc/profile macht ihn beim Boot 0777,
# ind setzt ihn bei jeder vollen Prüfung wieder auf 0755
chown -h 10009:10009 "$D" || echo "$(date) chown $D fehlgeschlagen" >> "$L"
# Inode einmal merken: ab jetzt gilt nur noch dieser Ordner als flexkodi
DINO=$(dino)
[ -n "$DINO" ] || bridge_off "$D ist kein Ordner des Launchers (Inode/Besitzer)"
( ind && rm -f start stop )
rtdir
echo "$(date) kodi-watch gestartet: $(ls -ldn "$D"), Brücke $([ -e "$R/bridge-off" ] && echo aus || echo an)" >> "$L"
n=0
while :; do
  # Takt (siehe Kopf): per = Pflichtrunde (Anlauf nach dem Start, sonst jede IDLEN-te Runde),
  # pend = Anforderung oder falsches running liegt; beides mit eingebauten Tests, ohne Prozess
  n=$((n + 1))
  per=""
  { [ "$n" -le "$BOOTN" ] || [ $((n % IDLEN)) -eq 0 ]; } && per=1
  pend=""
  fkpending && pend=1
  # ohne Laufschlüssel beginnt die Zählung "Runden nach bereit" für den nächsten neu (auch in
  # Runden, in denen rtdir nicht läuft)
  [ -e "$RT/b/key" ] || [ -L "$RT/b/key" ] || KEYAGE=0
  # Brücke voll prüfen: Pflichtrunde, vor jeder Anforderung (also vor kodi.sh start) und solange
  # Kodi läuft oder ein Laufschlüssel liegt
  if [ -n "$per$pend" ] || kodi_busy; then rtdir; fi
  # flexkodi (cd, Inode, chmod 755, start/stop, running): Pflichtrunde oder Anforderung. Solange
  # Kodi läuft und die Brücke an ist, prüft rtdir (Schritt 3) den Ordner ohnehin jede Runde.
  req=""
  if [ -n "$per$pend" ]; then
    if req=$(fkround); then
      FKBAD=""
    else
      [ -n "$FKBAD" ] || { FKBAD=1; echo "$(date) $D ist nicht der geprüfte Ordner, Anforderungen ruhen" >> "$L"; bridge_off "$D getauscht"; }
      req=""
    fi
  fi
  if [ "$req" = stop ]; then
    echo "$(date) Stop angefordert" >> "$L"
    "$K/kodi.sh" stop >> "$L" 2>&1
    # eine Startanforderung, die während des Stopps kam, gleich behandeln (wie bisher)
    req=$(fkround start) || req=""
  fi
  case "$req" in
    start*)
      t=${req#start}
      t=${t# }
      now=$(date +%s)
      case "$t" in
        ''|*[!0-9]*) echo "$(date) Anforderung ungültig, verworfen" >> "$L" ;;
        *)
          if [ $((now - t)) -le 5 ] && [ $((t - now)) -le 5 ]; then
            echo "$(date) Start angefordert" >> "$L"
            # Kachel-Start: flex.js hat die freie Startseite geprüft, kodi.sh darf die
            # pausierten Hintergrund-Apps beenden (close_apps)
            [ -x "$K/kodi.sh" ] && FLEX_KODI_FROM_TILE=1 "$K/kodi.sh" start >> "$L" 2>&1
          else
            echo "$(date) Anforderung zu alt ($t, jetzt $now), verworfen" >> "$L"
          fi ;;
      esac ;;
  esac
  sleep 1
done
