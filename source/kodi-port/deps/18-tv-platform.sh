#!/bin/bash
# TV-Plattform in den Sysroot legen: DirectFB-1.4.2-Header (+ erzeugte), DirectFB- und Mali-
# Bibliotheken des TVs zum Linken, Khronos-Header (nur EGL + GLES2: ohne GLES3-Header bleibt
# Kodi bei HAS_GLES=2, passend zum Mali-470), pkg-config-Dateien.
# Bibliotheken: ~/flex/sysroot-tv/lib (vom TV geholt). Die Mali-Kopien heißen libEGL/libGLESv2,
# tragen aber den SONAME libMali.so -> am TV wird /mslib/libMali.so geladen.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done tv-platform && exit 0
TVLIB=${FLEX_BUILD_ROOT:-$HOME/flex}/sysroot-tv/lib
DFB=$SRC/directfb-1.4.2
GEN=${FLEX_BUILD_ROOT:-$HOME/flex}/sysroot-tv/include-dfb-gen
for f in libdirectfb.so libdirect.so libfusion.so libMali.so; do
  [ -f "$TVLIB/$f" ] || { echo "FEHLER: $TVLIB/$f fehlt"; exit 1; }
done

# DirectFB-Header: include/directfb/ wie bei einer normalen Installation
D=$PREFIX/include/directfb
rm -rf "$D" && mkdir -p "$D/direct" "$D/fusion"
cp "$DFB"/include/*.h "$D/"
cp "$DFB"/lib/direct/*.h "$D/direct/"
cp "$DFB"/lib/fusion/*.h "$D/fusion/"
cp "$GEN"/direct/build.h "$D/direct/"
cp "$GEN"/fusion/build.h "$D/fusion/"
cp "$GEN"/directfb_version.h "$GEN"/directfb_build.h "$D/"

# Bibliotheken
cp "$TVLIB/libdirectfb.so" "$TVLIB/libdirect.so" "$TVLIB/libfusion.so" "$PREFIX/lib/"
for n in libMali.so libEGL.so libGLESv2.so; do
  cp "$TVLIB/libMali.so" "$PREFIX/lib/$n"
  patchelf --set-soname libMali.so "$PREFIX/lib/$n"
done

# Khronos-Header
rm -rf "$PREFIX/include/EGL" "$PREFIX/include/GLES2" "$PREFIX/include/KHR" "$PREFIX/include/GLES3"
cp -r /usr/include/EGL /usr/include/GLES2 /usr/include/KHR "$PREFIX/include/"

P=$PREFIX/lib/pkgconfig
mkdir -p "$P"
cat > "$P/directfb.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: DirectFB
Description: DirectFB 1.4.2 (MStar, TV library for linking)
Version: 1.4.2
Libs: -L\${libdir} -ldirectfb -lfusion -ldirect
Cflags: -I\${includedir}/directfb
EOF
cat > "$P/egl.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: egl
Description: Mali-470 EGL (libMali)
Version: 1.4
Libs: -L\${libdir} -lEGL
Cflags: -I\${includedir} -DEGL_NO_X11 -DMESA_EGL_NO_X11_HEADERS
EOF
cat > "$P/glesv2.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: glesv2
Description: Mali-470 OpenGL ES 2.0 (libMali)
Version: 2.0
Libs: -L\${libdir} -lGLESv2
Cflags: -I\${includedir}
EOF
done_stamp tv-platform
