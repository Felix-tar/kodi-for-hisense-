#!/bin/bash
# libass 0.17.1 (Untertitel). Ohne fontconfig: Kodi gibt die Schriften selbst vor.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done libass && exit 0
fetch libass
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
  --disable-fontconfig --disable-require-system-font-provider --disable-asm --disable-libunibreak
make -j"$JOBS"
make install
done_stamp libass
