#!/bin/bash
set -eu
. "$(dirname "$0")/../lib.sh"
is_done zlib && exit 0
fetch zlib
cd "$S"
CHOST=$HOST ./configure --prefix="$PREFIX" --static
make -j"$JOBS"
make install
done_stamp zlib
