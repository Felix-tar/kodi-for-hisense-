#!/bin/bash
# nghttp2 1.46.0 (HTTP/2 für curl), nur die Bibliothek, Optionen wie Kodis depends. Kodis
# FindCurl verlangt sie bei statischem curl in jedem Fall. CMAKE_POLICY_VERSION_MINIMUM: das
# CMakeLists von 1.46 nennt eine Mindestversion, die CMake 4 nicht mehr annimmt.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done nghttp2 && exit 0
fetch nghttp2
cmake -S "$S" -B "$S/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_DEBUG=OFF -DENABLE_FAILMALLOC=OFF -DENABLE_LIB_ONLY=ON \
  -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF
ninja -C "$S/build" -j"$JOBS"
ninja -C "$S/build" install
done_stamp nghttp2
