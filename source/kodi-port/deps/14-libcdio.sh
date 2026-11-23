#!/bin/bash
# libcdio 2.1.0 (Kodi 21.3 verlangt es auch ohne CD-/DVD-Funktionen), nur die Bibliothek.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done libcdio && exit 0
fetch libcdio-gplv3
cd "$S"
# configure findet lseek64/fseeko64 in der glibc; Zigs Header deklarieren sie nur mit
# _LARGEFILE64_SOURCE
CPPFLAGS="${CPPFLAGS:-} -D_LARGEFILE64_SOURCE" ./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static --without-versioned-libs \
  --without-cd-drive --without-cd-info --without-cdda-player --without-cd-read --without-iso-info \
  --without-iso-read --disable-example-progs --disable-cpp-progs --disable-cddb --disable-vcd-info
make -j"$JOBS" -C lib
make -C lib install
make -C include install
make install-pkgconfigDATA 2>/dev/null || cp libcdio.pc "$PREFIX/lib/pkgconfig/" 2>/dev/null || true
done_stamp libcdio
