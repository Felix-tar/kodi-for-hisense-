#!/bin/bash
# Kodi bauen (nach configure-kodi.sh). -k 0: weiterbauen und alle Fehler auf einmal sammeln.
# Log: ~/flex/build-deps/logs/kodi-build.log, am Ende die Fehlerzusammenfassung.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
B=${FLEX_BUILD_ROOT:-$HOME/flex}/build-kodi
L=$LOGS/kodi-build.log
start=$(date +%s)
ninja -C "$B" -k 0 -j"$JOBS" "$@" > "$L" 2>&1
rc=$?
echo "ninja rc=$rc nach $(( $(date +%s) - start )) s"
echo "--- fehlgeschlagene Schritte: $(grep -c '^FAILED:' "$L")"
grep '^FAILED:' "$L" | sed 's/ .*-o / -> /; s/ .*//' | head -40
echo "--- erste Fehlermeldungen"
grep -n -m 40 -E 'error:|undefined reference|undefined symbol' "$L" | cut -c1-300
exit $rc
