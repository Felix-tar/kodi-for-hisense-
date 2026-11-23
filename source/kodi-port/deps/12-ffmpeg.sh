#!/bin/bash
# FFmpeg 6.0.1 (Version wie Kodi 21.3), Optionen wie in Kodis tools/depends/target/ffmpeg:
# GPL, postproc, Laufzeit-CPU-Erkennung, NEON, ohne Geräte/Programme/VAAPI/VDPAU/Vulkan.
# Ohne TLS (Kodi lädt Netzwerk-Daten über sein eigenes curl) und ohne dav1d (4K/AV1 kommt
# später über den Hardware-Player des TVs).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done ffmpeg && exit 0
fetch ffmpeg
cd "$S"
# Clang 21 (Zig 0.16) stürzt im ARM-Backend bei den NEON-Intrinsics dieser Datei ab ("Do not know
# how to split the result of this operator!", auch mit -O0). Sie beschleunigt nur die
# Dequantisierung von MPEG-1/2/4; ohne sie nimmt FFmpeg die C-Fassung.
cat > libavcodec/neon/mpegvideo.c <<'EOF'
/* FLEX-TV: NEON-Variante entfernt (Clang-21-Backend-Absturz), C-Fassung wird benutzt */
#include "libavutil/attributes.h"
#include "libavcodec/mpegvideo.h"

av_cold void ff_mpv_common_init_neon(MpegEncContext *s)
{
}
EOF
./configure --prefix="$PREFIX" --enable-cross-compile --target-os=linux --arch=arm --cpu=cortex-a7 \
  --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
  --pkg-config=pkg-config --enable-pic --enable-neon --disable-armv5te --disable-armv6t2 \
  --enable-static --disable-shared --disable-doc --disable-devices --disable-programs --disable-sdl2 \
  --disable-vulkan --disable-vaapi --disable-vdpau --disable-xlib --disable-libxcb --disable-libdrm \
  --enable-gpl --enable-postproc --enable-runtime-cpudetect --enable-pthreads --disable-debug \
  --extra-ldflags="$LDFLAGS" --extra-version=kodi-flex
# configure findet sysctl() in der glibc-2.18-Stub-Bibliothek, Zigs Header (neuere glibc) haben
# aber kein sys/sysctl.h mehr; unter Linux nutzt FFmpeg es ohnehin nicht.
sed -i 's/^#define HAVE_SYSCTL 1$/#define HAVE_SYSCTL 0/' config.h
make -j"$JOBS"
make install
done_stamp ffmpeg
