#!/bin/bash
# Welche Hilfspakete sind auf dem Build-Rechner installiert? (für die Anleitung)
for p in build-essential cmake ninja-build autoconf automake libtool libtool-bin pkgconf \
  pkg-config patchelf git curl binutils-arm-linux-gnueabi ccache meson default-jre-headless \
  libegl-dev libgles-dev xz-utils bzip2 python3 gperf nasm; do
  if dpkg-query -W -f='${Status}\n' "$p" 2>/dev/null | grep -q 'install ok installed'; then
    echo "$p ja"
  else
    echo "$p nein"
  fi
done
