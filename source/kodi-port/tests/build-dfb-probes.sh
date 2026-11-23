#!/bin/bash
# Baut die kleinen DirectFB-Proben aus kodi-port/tests für den TV nach ~/flex/out
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
mkdir -p "${FLEX_BUILD_ROOT:-$HOME/flex}/out"
for p in dfb-struct-probe dfb-layer-probe dfb-layer-color-probe dfb-window-probe; do
  "$CC" -O2 -std=gnu11 -Wall -I"$PREFIX/include/directfb" "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/tests/$p.c" \
    -L"$PREFIX/lib" -ldirectfb -ldirect -lfusion -Wl,--allow-shlib-undefined $LDFLAGS \
    -o "${FLEX_BUILD_ROOT:-$HOME/flex}/out/$p" && echo "$p ok"
done
