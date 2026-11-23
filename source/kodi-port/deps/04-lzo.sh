#!/bin/bash
set -eu
. "$(dirname "$0")/../lib.sh"
is_done lzo && exit 0
fetch liblzo2
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static
make -j"$JOBS"
make install
done_stamp lzo
