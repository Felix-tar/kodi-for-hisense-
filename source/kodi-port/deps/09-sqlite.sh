#!/bin/bash
set -eu
. "$(dirname "$0")/../lib.sh"
is_done sqlite && exit 0
fetch sqlite3
cd "$S"
CFLAGS="$CFLAGS -DSQLITE_TEMP_STORE=3 -DSQLITE_DEFAULT_MMAP_SIZE=268435456 -DSQLITE_ENABLE_COLUMN_METADATA=1" \
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static --disable-readline \
  --disable-editline --enable-threadsafe
make -j"$JOBS"
make install
done_stamp sqlite
