#!/bin/bash
set -eu
. "$(dirname "$0")/../lib.sh"
is_done fribidi && exit 0
fetch fribidi
cd "$S"
meson setup build --cross-file "$TC/meson-cross.ini" --prefix="$PREFIX" --libdir=lib \
  -Ddefault_library=static -Dbuildtype=release -Ddocs=false -Dbin=false -Dtests=false
ninja -C build -j"$JOBS"
ninja -C build install
done_stamp fribidi
