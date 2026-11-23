#!/bin/bash
# Disassembliert die Funktion um eine Adresse im ungestrippten kodi-directfb.
# Aufruf: disasm.sh <funktionsname-teil> [adresse]
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
B=${FLEX_BUILD_ROOT:-$HOME/flex}/build-kodi/kodi-directfb
s=$("/usr/bin/$HOST-nm" -D -C -n "$B" | grep -m1 -F " $1")
echo "Symbol: $s"
start=0x${s%% *}
"/usr/bin/$HOST-objdump" -d -C --start-address=$start --stop-address=$((start + 0x260)) "$B" \
  | grep -v '^$' | sed -n '1,200p'
