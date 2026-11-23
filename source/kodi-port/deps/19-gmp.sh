#!/bin/bash
# GMP 6.2.1 (Version wie Kodi 21.3, für nettle/hogweed -> gnutls -> libsmbclient), nur statisch.
# Optionen wie Kodis tools/depends/target/gmp: für Linux/ARM nur --disable-shared, die
# ARM-Assembler-Routinen bleiben an.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done gmp && exit 0
fetch gmp
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static
make -j"$JOBS"
make install
"$NM" "$PREFIX/lib/libgmp.a" | grep -q ' T __gmpz_init$' || { echo "FEHLER: __gmpz_init fehlt in libgmp.a"; exit 1; }
done_stamp gmp
