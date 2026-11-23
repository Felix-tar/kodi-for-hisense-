#!/bin/bash
# OpenSSL 1.1.1w (Version wie Kodi 21.3), nur statische Bibliotheken.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done openssl && exit 0
fetch openssl
cd "$S"
# CC kommt aus der Umgebung (Zig-Wrapper); kein CROSS_COMPILE-Präfix zusätzlich
./Configure linux-armv4 no-shared no-tests no-dso no-engine no-async no-afalgeng -fPIC \
  --prefix="$PREFIX" --openssldir=/etc/ssl --libdir=lib
make -j"$JOBS" build_libs
make install_dev
done_stamp openssl
