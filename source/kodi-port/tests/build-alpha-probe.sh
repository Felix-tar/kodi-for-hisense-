#!/bin/bash
# Baut native/flex_alpha_probe.c für den TV nach ~/flex/out/flex-alpha-probe (wie der M1-Test,
# gegen die Link-Kopien im Kodi-Sysroot)
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
mkdir -p "${FLEX_BUILD_ROOT:-$HOME/flex}/out"
"$CC" -O2 -std=gnu11 -Wall -DEGL_NO_X11 -I"$PREFIX/include/directfb" -I"$PREFIX/include" \
  "${FLEX_BUILD_ROOT:-$HOME/flex}/native-src/flex_alpha_probe.c" \
  -L"$PREFIX/lib" -ldirectfb -ldirect -lfusion -lMali -Wl,--allow-shlib-undefined $LDFLAGS \
  -o "${FLEX_BUILD_ROOT:-$HOME/flex}/out/flex-alpha-probe" && file "${FLEX_BUILD_ROOT:-$HOME/flex}/out/flex-alpha-probe"
