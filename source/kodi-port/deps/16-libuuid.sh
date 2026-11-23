#!/bin/bash
# libuuid aus e2fsprogs 1.46.5 (für crossguid), nur lib/uuid wie in Kodis depends. Die eigene
# blkid bleibt eingeschaltet (--disable-libblkid verlangt eine externe), gebaut wird sie nicht.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done libuuid && exit 0
fetch libuuid
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-fsck --enable-libuuid \
  --disable-nls --disable-uuidd --disable-defrag --disable-imager --disable-resizer --disable-debugfs \
  --disable-e2initrd-helper --disable-elf-shlibs
make -C lib/uuid -j1
# MKDIR_P von e2fsprogs legt den Header-Ordner beim Cross-Build nicht an
mkdir -p "$PREFIX/include/uuid" "$PREFIX/lib/pkgconfig"
make -C lib/uuid -j1 install
done_stamp libuuid
