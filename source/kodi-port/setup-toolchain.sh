#!/bin/bash
# Toolchain-Wrapper, CMake-Toolchain-Datei und Meson-Cross-Datei anlegen (idempotent).
set -eu
. "$(dirname "$0")/env.sh"
sudo -n apt-get install -y -q binutils-arm-linux-gnueabi >/dev/null 2>&1 || true
command -v $HOST-ar >/dev/null || { echo "FEHLER: $HOST-ar fehlt (binutils-arm-linux-gnueabi)"; exit 1; }
mkdir -p "$TC/bin"
TARGET="-target arm-linux-gnueabi.2.18 -mcpu=cortex_a7 -mfloat-abi=softfp"
# CPU legt der Wrapper fest; -mcpu/-march/-mtune der Pakete fallen weg (Zig liest z. B.
# "-mcpu=cortex-a7" als CPU "cortex" ohne Merkmal "a7" und bricht ab).
# Kodis dll-loader linkt Shared Libraries mit --unresolved-symbols=ignore-all; Zig kennt die
# Option nicht, das gleichwertige "-z undefs" schon.
# Ohne Optimierung (kein -O oder -O0, z. B. Kodis Mime.cpp) schaltet Zig den UB-Sanitizer ein,
# dessen Laufzeit beim Linken fehlt: dann -fno-sanitize=undefined. Die Befehlszeilen im Build
# bleiben gleich, Ninja übersetzt deshalb nur neu, was es neu übersetzen muss.
for w in gcc:cc g++:c++; do
  cat > "$TC/bin/$HOST-${w%%:*}" <<EOF
#!/bin/bash
args=()
opt=0
for a in "\$@"; do
  case "\$a" in
    -mcpu=*|-march=*|-mtune=*) ;;
    -Wl,--unresolved-symbols=ignore-all) args+=("-Wl,-z,undefs") ;;
    -O0) opt=0; args+=("\$a") ;;
    -O*) opt=1; args+=("\$a") ;;
    *) args+=("\$a") ;;
  esac
done
[ \$opt = 0 ] && args+=("-fno-sanitize=undefined")
exec "$ZIG" ${w##*:} $TARGET "-ffile-prefix-map=$F=/usr/src/flex" "\${args[@]}"
EOF
done
# Zigs glibc-Header deklarieren gettid() ohne Versionsprüfung (andere neue Funktionen wie
# getrandom prüfen __GLIBC_MINOR__). Die TV-glibc 2.18 hat gettid nicht (erst 2.30), und Kodis
# eigener Ersatz für alte glibc kollidiert dann mit der Deklaration.
U=$(dirname "$ZIG")/lib/libc/include/generic-glibc/bits/unistd_ext.h
if ! grep -q 'FLEX-TV' "$U"; then
  sed -i 's|^extern __pid_t gettid (void) __THROW;$|/* FLEX-TV: erst ab glibc 2.30 */\n#if __GLIBC_MINOR__ >= 30\nextern __pid_t gettid (void) __THROW;\n#endif|' "$U"
  grep -q 'FLEX-TV' "$U" || { echo "FEHLER: gettid-Deklaration in $U nicht gefunden"; exit 1; }
fi

ln -sf "$HOST-gcc" "$TC/bin/$HOST-cc"
ln -sf "$HOST-g++" "$TC/bin/$HOST-c++"
chmod 755 "$TC/bin/$HOST-gcc" "$TC/bin/$HOST-g++"

cat > "$TC/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $TC/bin/$HOST-gcc)
set(CMAKE_CXX_COMPILER $TC/bin/$HOST-g++)
set(CMAKE_AR /usr/bin/$HOST-ar CACHE FILEPATH "")
set(CMAKE_RANLIB /usr/bin/$HOST-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP /usr/bin/$HOST-strip CACHE FILEPATH "")
set(CMAKE_NM /usr/bin/$HOST-nm CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT "-fPIC")
set(CMAKE_CXX_FLAGS_INIT "-fPIC")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-T,$LDSCRIPT")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-T,$LDSCRIPT")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
EOF

cat > "$TC/meson-cross.ini" <<EOF
[binaries]
c = '$TC/bin/$HOST-gcc'
cpp = '$TC/bin/$HOST-g++'
ar = '/usr/bin/$HOST-ar'
strip = '/usr/bin/$HOST-strip'
pkg-config = 'pkg-config'

[built-in options]
c_link_args = ['-Wl,-T,$LDSCRIPT']
cpp_link_args = ['-Wl,-T,$LDSCRIPT']

[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a7'
endian = 'little'
EOF

# Schnelltest: C und C++ übersetzen
echo 'int main(void){return 0;}' > /tmp/t.c
echo '#include <string>
int main(){std::string s("x");return (int)s.size()-1;}' > /tmp/t.cpp
$CC $LDFLAGS /tmp/t.c -o /tmp/t-c && $CXX $LDFLAGS /tmp/t.cpp -o /tmp/t-cxx && file /tmp/t-c /tmp/t-cxx
