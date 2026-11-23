#!/bin/bash
# FreeType 2.13.2 ohne HarfBuzz (HarfBuzz baut danach gegen dieses FreeType).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done freetype && exit 0
fetch freetype2
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
  --with-zlib=yes --without-harfbuzz --without-png --without-bzip2 --without-brotli
make -j"$JOBS"
make install
done_stamp freetype
