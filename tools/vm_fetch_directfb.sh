#!/bin/bash
# Fetch the pinned DirectFB 1.4.2 archive used to generate public headers.
set -eu
F=${FLEX_BUILD_ROOT:-$HOME/flex}
archive="$F/dl/DirectFB-1.4.2.tar.gz"
target="$F/src/directfb-1.4.2"
checksum=051847e21e88a9c32ee78f8920014602d8b21928602075c5266433cd58addbc6
mkdir -p "$F/dl" "$F/src"
if [ ! -f "$archive" ]; then
  curl -fsSL -o "$archive.download" https://sources.buildroot.net/DirectFB-1.4.2.tar.gz
  printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum -c -
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
if [ ! -f "$target/include/directfb.h" ]; then
  mkdir -p "$target"
  tar -xzf "$archive" -C "$target" --strip-components=1
fi
test -f "$target/include/directfb.h"
