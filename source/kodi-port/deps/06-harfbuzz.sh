#!/bin/bash
set -eu
. "$(dirname "$0")/../lib.sh"
is_done harfbuzz && exit 0
fetch harfbuzz
cd "$S"
meson setup build --cross-file "$TC/meson-cross.ini" --prefix="$PREFIX" --libdir=lib \
  -Ddefault_library=static -Dbuildtype=release -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled \
  -Dcairo=disabled -Dchafa=disabled -Dicu=disabled -Dgraphite2=disabled -Dtests=disabled \
  -Ddocs=disabled -Dintrospection=disabled -Dutilities=disabled
ninja -C build -j"$JOBS"
ninja -C build install
done_stamp harfbuzz
