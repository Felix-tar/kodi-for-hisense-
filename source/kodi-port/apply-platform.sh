#!/bin/bash
# Baut die DirectFB-Plattform (kodi-tree/) in den Kodi-21.3-Quellbaum ein und schreibt den
# Unterschied zum Original als Patch nach patches/kodi-21.3-directfb.patch (für den Guide).
# Wiederholbar: was Original ist, weiß git (der Quellbaum ist ein Klon von 21.3-Omega).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
K=$KODI_SRC
T=$HERE/kodi-tree
P=$HERE/patches/kodi-21.3-directfb.patch
mkdir -p "$HERE/patches"
git -C "$K" rev-parse --git-dir > /dev/null || { echo "FEHLER: $K ist kein git-Klon"; exit 1; }
# Reste der früheren Fassung dieses Skripts
find "$K" -name '*.flex-orig' -delete
# Sicherungskopien neben den Dateien (*.pre-tvplayer) gehören nicht in den Kodi-Baum und den Patch
files=$(cd "$T" && find . -type f ! -name '*.pre-tvplayer' | sed 's|^\./||' | LC_ALL=C sort)
for f in $files; do
  mkdir -p "$(dirname "$K/$f")"
  cp "$T/$f" "$K/$f"
done
: > "$P"
for f in $files; do
  if git -C "$K" ls-files --error-unmatch "$f" > /dev/null 2>&1; then
    git -C "$K" diff -- "$f" >> "$P"
  else
    (cd "$K" && git diff --no-index -- /dev/null "$f") >> "$P" || true
  fi
done
echo "Plattform eingebaut, Patch: $P ($(grep -c '^+++ ' "$P") Dateien, $(wc -l < "$P") Zeilen)"
