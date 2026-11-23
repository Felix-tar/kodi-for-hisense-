#!/bin/bash
# Builds and runs the host test of the receiver status parser (FlexDenonStatus.cpp) on the build VM,
# host compiler, all warnings as errors.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../../kodi-tree/xbmc/windowing/directfb
OUT=${TMPDIR:-/tmp}/flex_volume_test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$SRC" -o "$OUT" "$HERE/denon_status_test.cpp" "$SRC/FlexDenonStatus.cpp"
"$OUT"
