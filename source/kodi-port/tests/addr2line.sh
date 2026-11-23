#!/bin/bash
# Absturzadressen aus dem TV-Log (libSegFault) gegen das ungestrippte kodi-directfb auflösen.
# Aufruf: addr2line.sh <adresse>...
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
B=${FLEX_BUILD_ROOT:-$HOME/flex}/build-kodi/kodi-directfb
"/usr/bin/$HOST-readelf" -h "$B" | grep -E 'Type:|Entry'
for a in "$@"; do
  echo "== $a"
  "/usr/bin/$HOST-addr2line" -f -C -i -e "$B" "$a" | sed 's/^/   /'
done
