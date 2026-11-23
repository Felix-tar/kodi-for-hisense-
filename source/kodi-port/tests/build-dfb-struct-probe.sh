#!/bin/bash
# Baut tests/dfb-struct-probe.c für den TV nach ~/flex/out/dfb-struct-probe
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
mkdir -p "${FLEX_BUILD_ROOT:-$HOME/flex}/out"
"$CC" -O2 -std=gnu11 -Wall -I"$PREFIX/include/directfb" \
  "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/tests/dfb-struct-probe.c" \
  -L"$PREFIX/lib" -ldirectfb -ldirect -lfusion -Wl,--allow-shlib-undefined $LDFLAGS \
  -o "${FLEX_BUILD_ROOT:-$HOME/flex}/out/dfb-struct-probe"
file "${FLEX_BUILD_ROOT:-$HOME/flex}/out/dfb-struct-probe"
