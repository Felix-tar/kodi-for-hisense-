#!/bin/bash
# TinyXML v1 2.6.2 (Kodi-Mirror-Paket mit autotools, wie Kodis depends: autoreconf, nur src/).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done tinyxml && exit 0
fetch tinyxml
cd "$S"
autoreconf -vif >/dev/null
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static
make -j1 -C src
make -C src install
done_stamp tinyxml
