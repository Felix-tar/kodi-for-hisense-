#!/bin/bash
# Generate the DirectFB 1.4.2 headers consumed by deps/18-tv-platform.sh.
set -eu
F=${FLEX_BUILD_ROOT:-$HOME/flex}
DFB="$F/src/directfb-1.4.2"
GEN="$F/sysroot-tv/include-dfb-gen"
test -f "$DFB/include/directfb.h"
mkdir -p "$GEN/direct" "$GEN/fusion"
sed -e 's/@DIRECT_BUILD_DEBUG@/0/;s/@DIRECT_BUILD_DEBUGS@/0/;s/@DIRECT_BUILD_TRACE@/0/;s/@DIRECT_BUILD_TEXT@/1/' \
    -e 's/@DIRECT_BUILD_GETTID@/1/;s/@DIRECT_BUILD_NETWORK@/1/;s/@DIRECT_BUILD_STDBOOL@/1/' \
    "$DFB/lib/direct/build.h.in" > "$GEN/direct/build.h"
sed -e 's/@FUSION_BUILD_MULTI@/1/;s/@FUSION_BUILD_KERNEL@/1/;s/@FUSION_MESSAGE_SIZE@/16384/' \
    "$DFB/lib/fusion/build.h.in" > "$GEN/fusion/build.h"
sed -e 's/@DIRECTFB_MAJOR_VERSION@/1/;s/@DIRECTFB_MINOR_VERSION@/4/;s/@DIRECTFB_MICRO_VERSION@/2/' \
    -e 's/@DIRECTFB_BINARY_AGE@/2/;s/@DIRECTFB_INTERFACE_AGE@/0/' \
    "$DFB/include/directfb_version.h.in" > "$GEN/directfb_version.h"
touch "$GEN/directfb_build.h"
