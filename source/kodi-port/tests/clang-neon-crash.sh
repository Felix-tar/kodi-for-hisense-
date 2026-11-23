#!/bin/bash
# Welche Optionen umgehen den Clang-21-Backend-Absturz bei libavcodec/neon/mpegvideo.c?
. "$(dirname "$0")/../env.sh"
P=$(ls -t /tmp/mpegvideo-*.c 2>/dev/null | head -1)
[ -n "$P" ] || { echo "keine vorverarbeitete Datei"; exit 1; }
for opt in "-O2" "-O1" "-O2 -fno-slp-vectorize" "-O2 -fno-vectorize -fno-slp-vectorize" "-O2 -mfloat-abi=softfp -mfpu=neon" "-Os"; do
  if $CC -c $opt -w "$P" -o /tmp/mv-test.o >/dev/null 2>&1; then r=ok; else r=ABSTURZ; fi
  printf '%-45s %s\n' "$opt" "$r"
done
