#!/bin/bash
# Baut transcode_test für den TV (ARMv7, glibc 2.18) gegen dieselbe statische FFmpeg-Fassung wie Kodi.
# Run on the Linux build host after building the required FFmpeg libraries.
#   bash ~/flex/kodi-port/tests/tvplayer/transcode/build.sh
# Ergebnis: ~/flex/transcode_test (vom PC mit tools/vmctl.py get holen, mit tvctl.py put nach /tmp).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../../../env.sh"
TV=$HERE/../../../kodi-tree/xbmc/cores/TVPlayer
OUT=${FLEX_BUILD_ROOT:-$HOME/flex}/transcode_test
LIBS=$(pkg-config --static --libs libavformat libavcodec libswresample libavutil)
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$PREFIX/include" -I"$TV" \
  "$HERE/transcode_test.cpp" "$TV/TVAudioTranscoder.cpp" -o "$OUT" $LDFLAGS $LIBS -lpthread -lm
/usr/bin/$HOST-strip "$OUT"
ls -la "$OUT"
md5sum "$OUT"
