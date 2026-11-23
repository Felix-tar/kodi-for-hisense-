#!/bin/bash
# Build-VM einrichten (idempotent): Zig 0.16.0 (gleiche Version wie auf dem Windows-PC, auf dem TV
# geprüft) nach ~/flex/zig, Kodi 21.3-Omega nach ~/flex/src/kodi, Khronos-Header (EGL/GLES2).
set -e
B=${FLEX_BUILD_ROOT:-$HOME/flex}
mkdir -p "$B/src" "$B/dl"
cd "$B/dl"

ZV=0.16.0
if [ ! -x "$B/zig/zig" ]; then
  curl -fsSL -o index.json https://ziglang.org/download/index.json
  read -r URL SHA < <(python3 -c "import json;d=json.load(open('index.json'))['$ZV']['x86_64-linux'];print(d['tarball'],d['shasum'])")
  F=$(basename "$URL")
  [ -f "$F" ] || curl -fsSL -o "$F" "$URL"
  echo "$SHA  $F" | sha256sum -c -
  rm -rf "$B/zig" && mkdir -p "$B/zig"
  tar -xJf "$F" -C "$B/zig" --strip-components=1
fi
"$B/zig/zig" version

if [ ! -d "$B/src/kodi/.git" ]; then
  git clone --depth 1 --branch 21.3-Omega https://github.com/xbmc/xbmc.git "$B/src/kodi"
fi
git -C "$B/src/kodi" describe --tags

# Khronos-Header (nur Header, gegen libMali des TVs wird gelinkt)
if [ ! -f /usr/include/EGL/egl.h ] || [ ! -f /usr/include/GLES2/gl2.h ]; then
  sudo -n apt-get install -y -q libegl-dev libgles-dev >/dev/null
fi
ls /usr/include/EGL/egl.h /usr/include/GLES2/gl2.h
df -h "$B" | tail -1
