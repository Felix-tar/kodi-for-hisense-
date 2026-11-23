#!/bin/bash
# Kodis Hilfsprogramme für den Build-Rechner (laufen auf der VM, nicht am TV): flatc 23.3.3,
# JsonSchemaBuilder, TexturePacker -> ~/flex/native/bin. Mit dem Host-Compiler, NICHT mit der
# TV-Toolchain (darum hier kein env.sh).
set -eu
F=${FLEX_BUILD_ROOT:-$HOME/flex}
KODI=$F/src/kodi
N=$F/native
B=$F/build-native
mkdir -p "$N/bin" "$B"
J=${JOBS:-4}

if [ ! -x "$N/bin/flatc" ]; then
  # Quellen wie die Zielbibliothek: Version/SHA512 aus Kodis depends, Kodi-Mirror
  vf=$(ls "$KODI"/tools/depends/target/flatbuffers/*-VERSION)
  eval "$(sed -n -E 's/\$\(([A-Z_]+)\)/${\1}/g; s/^(LIBNAME|VERSION|SOURCE|ARCHIVE|SHA512|BASE_URL)=([^\\]*)$/\1="\2"/p' "$vf")"
  A=$F/dl/$ARCHIVE
  [ -f "$A" ] || curl -fsSL -o "$A" "https://mirrors.kodi.tv/build-deps/sources/$ARCHIVE"
  echo "$SHA512  $A" | sha512sum -c --status - || { echo "FEHLER: SHA512 flatbuffers"; exit 1; }
  rm -rf "$B/flatbuffers" && mkdir -p "$B/flatbuffers" && tar -xf "$A" -C "$B/flatbuffers" --strip-components=1
  cmake -S "$B/flatbuffers" -B "$B/flatbuffers/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF >/dev/null
  ninja -C "$B/flatbuffers/build" -j"$J" flatc >/dev/null
  cp "$B/flatbuffers/build/flatc" "$N/bin/"
fi
"$N/bin/flatc" --version

for t in JsonSchemaBuilder TexturePacker; do
  if [ ! -x "$N/bin/$t" ]; then
    # ARCH_DEFINES wie in Kodis depends (NATIVE_ARCH_DEFINES für einen Linux-Build-Rechner)
    cmake -S "$KODI/tools/depends/native/$t/src" -B "$B/$t" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DKODI_SOURCE_DIR="$KODI" -DCMAKE_INSTALL_PREFIX="$N" \
      "-DARCH_DEFINES=-DTARGET_POSIX;-DTARGET_LINUX;-D_LINUX" >/dev/null
    ninja -C "$B/$t" -j"$J" >/dev/null
    find "$B/$t" -maxdepth 2 -type f -executable -name "*$t*" -exec cp {} "$N/bin/$t" \;
  fi
  ls -l "$N/bin/$t"
done
