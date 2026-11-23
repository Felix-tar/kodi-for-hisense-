#!/bin/bash
# Prüft: (1) mit welchen Optionen Zig Kodis NEON-Inline-Assembler (MatrixGL.neon.cpp) annimmt,
# (2) ob unistd.h gettid() für glibc 2.18 deklariert (sollte nicht: gibt es erst ab 2.30)
. "${FLEX_BUILD_ROOT:-$HOME/flex}/kodi-port/env.sh"
T=$(mktemp -d)
M=$KODI_SRC/xbmc/rendering/MatrixGL.neon.cpp
try() {
  "$ZIG" c++ -target arm-linux-gnueabi.2.18 "$@" -c "$M" -o "$T/m.o" 2>"$T/err"
  echo "rc=$? $* :: $(grep -m1 error "$T/err" | cut -c1-120)"
}
try -mcpu=cortex_a7 -mfloat-abi=softfp
try -mcpu=cortex_a7+neon -mfloat-abi=softfp
try -mcpu=cortex_a7 -mfloat-abi=softfp -mfpu=neon
try -mcpu=cortex_a7 -mfloat-abi=softfp -Xclang -target-feature -Xclang +neon
try -mcpu=cortex_a7+neon+d32 -mfloat-abi=softfp
try -mcpu=cortex_a7+neon+vfp4+d32 -mfloat-abi=softfp
try -mcpu=generic+v7a+neon -mfloat-abi=softfp
cat > "$T/g.c" <<'EOF'
#define _GNU_SOURCE
#include <unistd.h>
static int gettid(void) { return 1; }
int main(void) { return gettid(); }
EOF
"$CC" -c "$T/g.c" -o "$T/g.o" 2>/dev/null; echo "gettid-Konflikt rc=$? (0 = kein Konflikt)"
rm -rf "$T"
