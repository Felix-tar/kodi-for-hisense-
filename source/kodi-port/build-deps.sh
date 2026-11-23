#!/bin/bash
# Alle Abhängigkeiten der Reihe nach bauen (fertige werden übersprungen). Je Paket ein Log in
# $LOGS; beim ersten Fehler stoppen und das Log-Ende zeigen. Übersprungene Schritte geben nichts
# aus und lassen ihr altes Log stehen.
# Aufruf: build-deps.sh [nur-diese-nummer-oder-name]
set -u
. "$(dirname "$0")/lib.sh"
cd "$(dirname "$0")/deps"
for s in [0-9][0-9]-*.sh; do
  [ -n "${1:-}" ] && [[ "$s" != *"$1"* ]] && continue
  n=${s%.sh}
  printf '%-22s ' "$n"
  start=$(date +%s)
  if bash "./$s" > "$LOGS/$n.log.new" 2>&1; then
    if [ -s "$LOGS/$n.log.new" ]; then
      mv "$LOGS/$n.log.new" "$LOGS/$n.log"
      echo "ok ($(( $(date +%s) - start )) s)"
    else
      rm -f "$LOGS/$n.log.new"
      echo "ok (fertig)"
    fi
  else
    mv "$LOGS/$n.log.new" "$LOGS/$n.log"
    echo "FEHLER (Log: $LOGS/$n.log)"
    tail -25 "$LOGS/$n.log"
    exit 1
  fi
done
echo "--- installierte Bibliotheken"
ls "$PREFIX/lib"/*.a
du -sh "$PREFIX"
