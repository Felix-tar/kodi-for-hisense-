#!/bin/bash
# libmicrohttpd 0.9.77 für Kodis Webserver (u. a. für die spätere Übergabe an den TV-Player),
# ohne HTTPS.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done libmicrohttpd && exit 0
fetch libmicrohttpd
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static --disable-https \
  --disable-curl --disable-doc --disable-examples --without-gnutls
make -j"$JOBS" -C src/microhttpd
make -C src/microhttpd install
make -C src/include install
make install-pkgconfigDATA
done_stamp libmicrohttpd
