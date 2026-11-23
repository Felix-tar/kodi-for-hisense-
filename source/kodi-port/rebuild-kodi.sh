#!/bin/bash
# Nach Änderungen an Toolchain oder Plattform: Wrapper neu schreiben, Plattform neu einspielen,
# die übergebenen Objektdateien (relativ zu ~/flex/build-kodi) löschen und weiterbauen.
# Beispiel: rebuild-kodi.sh build/utils/CMakeFiles/utils.dir/Mime.cpp.o
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
bash "$HERE/setup-toolchain.sh" > /dev/null 2>&1
bash "$HERE/apply-platform.sh"
for o in "$@"; do rm -f "${FLEX_BUILD_ROOT:-$HOME/flex}/build-kodi/$o"; done
exec bash "$HERE/build-kodi.sh"
