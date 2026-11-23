# Gemeinsame Umgebung für den Kodi-Cross-Build auf der Build-VM (wird von den Skripten gesourct).
# Ziel: Hisense 55U8QF, ARMv7 (Cortex-A55 im 32-Bit-Modus), glibc 2.18, softfp.
F=${FLEX_BUILD_ROOT:-$HOME/flex}
export ZIG=$F/zig/zig
export ZIG_GLOBAL_CACHE_DIR=$F/zig-cache
export TC=$F/tc
export HOST=arm-linux-gnueabi
export PREFIX=$F/sysroot-kodi        # statische Bibliotheken + Header für Kodi
export SRC=$F/src
export DL=$F/dl
export BUILD=$F/build-deps
export LOGS=$BUILD/logs
export KODI_SRC=$SRC/kodi
export LDSCRIPT=$F/native/glibc218-relocations.ld
export JOBS=${JOBS:-4}

export CC=$TC/bin/$HOST-gcc
export CXX=$TC/bin/$HOST-g++
export AR=/usr/bin/$HOST-ar
export RANLIB=/usr/bin/$HOST-ranlib
export STRIP=/usr/bin/$HOST-strip
export NM=/usr/bin/$HOST-nm
export CFLAGS="-O2 -fPIC"
export CXXFLAGS="-O2 -fPIC"
export CPPFLAGS="-I$PREFIX/include"
# Linker-Skript: ohne es stürzen Programme mit gemeinsamen Bibliotheken im Lader der TV-glibc ab
export LDFLAGS="-L$PREFIX/lib -Wl,-T,$LDSCRIPT"
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_PATH=
export PATH=$TC/bin:$PATH
# CMake 4 nimmt cmake_minimum_required(< 3.5) nicht mehr an; betrifft ältere Pakete und Kodis
# interne Abhängigkeiten (rapidjson, taglib, spdlog), die als ExternalProject gebaut werden
export CMAKE_POLICY_VERSION_MINIMUM=3.5

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$SRC" "$DL" "$BUILD" "$LOGS"
