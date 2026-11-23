#!/bin/bash
# Kodi 21.3 für den TV konfigurieren: Plattform "directfb" (GLES 2), kleine Ausstattung (ohne
# Python, UPnP, AirTunes, optische Laufwerke, Tests), Bibliotheken aus $PREFIX, Hilfsprogramme
# aus ~/flex/native/bin. Ergebnis: ~/flex/build-kodi (Ninja). Installationsziel am TV: /APPS/kodi.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"
F=${FLEX_BUILD_ROOT:-$HOME/flex}
B=$F/build-kodi
N=$F/native/bin
mkdir -p "$B"
# iconv steckt in der glibc. Kodis FindIconv sucht iconv.h und eine Bibliothek "c" nur im Sysroot
# und legt nur dann kein eigenes Ziel an, wenn der Pfad wie libc.* aussieht. Darum: Header aus
# Zigs glibc und ein leeres libc.a (der Linker nimmt die echte libc von Zig).
ICONV_DIR=$F/native/iconv
mkdir -p "$ICONV_DIR"
[ -f "$ICONV_DIR/libc.a" ] || "$AR" rcs "$ICONV_DIR/libc.a"
# Kodis interne Abhängigkeiten (crossguid, fmt, spdlog, ...) holen ihre Quellen vom Kodi-Mirror;
# libdvdread/libdvdnav werden mit autotools für --host=$HOST gebaut.
cmake -S "$KODI_SRC" -B "$B" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TC/toolchain.cmake" \
  `# klein (-Os), aber als Build-Typ "Release": bei MinSizeRel zeigen Kodis interne Bibliotheken` \
  `# (crossguid, spdlog, tinyxml2) auf ihre Debug-Varianten, die nie gebaut werden` \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
  -DCMAKE_INSTALL_PREFIX=/APPS/kodi \
  -DCORE_PLATFORM_NAME=directfb -DAPP_RENDER_SYSTEM=gles \
  -DWITH_CPU=cortex-a7 -DHOST_ARCH=$HOST \
  `# Kodis eigene Abhängigkeiten (fmt, spdlog, crossguid, ...) wie bei Kodis depends ins Sysroot:` \
  `# die Toolchain sucht nur dort, sonst findet z. B. spdlog das intern gebaute fmt nicht` \
  -DDEPENDS_PATH="$PREFIX" \
  -DENABLE_GOLD=OFF \
  `# Kodis NEON-Matrix (Inline-Assembler) nimmt Zigs Clang bei softfp nicht an, auch nicht mit` \
  `# +neon; die C-Fassung reicht für die GUI-Matrizen. FFmpeg nutzt NEON weiterhin.` \
  -DENABLE_NEON=OFF \
  -DENABLE_PYTHON=OFF -DENABLE_UPNP=OFF -DENABLE_AIRTUNES=OFF -DENABLE_OPTICAL=OFF \
  -DENABLE_DVDCSS=OFF -DENABLE_TESTING=OFF -DENABLE_EVENTCLIENTS=OFF \
  `# FindXSLT verlangt libxml2 hart (auch im AUTO-Modus); XSLT braucht nur das Skin-Werkzeug` \
  -DENABLE_XSLT=OFF \
  `# udfread (UDF-Images) baut Kodi sonst selbst, mit configure ohne --host` \
  -DENABLE_UDFREAD=OFF \
  -DICONV_INCLUDE_DIR="$(dirname "$ZIG")/lib/libc/include/generic-glibc" \
  -DICONV_LIBRARY="$ICONV_DIR/libc.a" \
  -DENABLE_INTERNAL_FFMPEG=OFF -DFFMPEG_PATH="$PREFIX" \
  -DENABLE_INTERNAL_CROSSGUID=ON -DENABLE_INTERNAL_FLATBUFFERS=ON -DENABLE_INTERNAL_FMT=ON \
  -DENABLE_INTERNAL_SPDLOG=ON -DENABLE_INTERNAL_RapidJSON=ON -DENABLE_INTERNAL_TAGLIB=ON \
  -DENABLE_INTERNAL_TINYXML2=ON -DENABLE_INTERNAL_KISSFFT=ON \
  -DENABLE_INTERNAL_PCRE=OFF -DENABLE_INTERNAL_FSTRCMP=OFF -DENABLE_INTERNAL_UDFREAD=OFF \
  -DWITH_TEXTUREPACKER="$N/TexturePacker" -DWITH_JSONSCHEMABUILDER="$N/JsonSchemaBuilder" \
  `# nur FindTexturePacker fragt das ab: mit dem VM-TexturePacker wird keiner für den TV gebaut` \
  -DHOST_CAN_EXECUTE_TARGET=TRUE -DINTERNAL_TEXTUREPACKER_INSTALLABLE=OFF \
  -DFLATBUFFERS_FLATC_EXECUTABLE="$N/flatc" \
  "$@"
