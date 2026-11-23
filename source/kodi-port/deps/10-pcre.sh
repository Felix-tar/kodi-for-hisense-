#!/bin/bash
# PCRE 8.45 (Kodi 21.3 nutzt PCRE1, nur die C-Bibliothek).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done pcre && exit 0
fetch pcre
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static --enable-utf \
  --enable-unicode-properties --enable-jit --disable-cpp --disable-pcregrep-libz --disable-pcregrep-libbz2
make -j"$JOBS" libpcre.la
make install-libLTLIBRARIES install-includeHEADERS install-nodist_includeHEADERS install-pkgconfigDATA
done_stamp pcre
