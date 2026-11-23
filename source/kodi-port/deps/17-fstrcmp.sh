#!/bin/bash
# fstrcmp 0.7.D001 (Kodis interne Variante ruft configure ohne --host auf, darum vorgebaut).
# fstrcmp ruft das libtool des Systems auf, das zum Linken den PC-gcc nimmt. Die Objekte
# übersetzt es aber mit unserem CC, darum packen wir sie selbst zur statischen Bibliothek.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done fstrcmp && exit 0
fetch fstrcmp
cd "$S"
autoreconf -vif >/dev/null 2>&1
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static
objs=$(ls lib/*.c lib/ac/*.c | sed 's/\.c$/.o/')
for o in $objs; do "$CC" $CFLAGS -I. -c "${o%.o}.c" -o "$o"; done
rm -f libfstrcmp.a && "$AR" rcs libfstrcmp.a $objs
install -m644 libfstrcmp.a "$PREFIX/lib/"
install -m644 lib/fstrcmp.h "$PREFIX/include/"
cat > "$PREFIX/lib/pkgconfig/fstrcmp.pc" <<PC
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: fstrcmp
Description: fuzzy string comparison
Version: $PKG_VERSION
Libs: -L\${libdir} -lfstrcmp
Cflags: -I\${includedir}
PC
"$NM" libfstrcmp.a | grep -q ' T fstrcmp$' || { echo "FEHLER: fstrcmp fehlt in libfstrcmp.a"; exit 1; }
done_stamp fstrcmp
