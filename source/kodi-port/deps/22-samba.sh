#!/bin/bash
# Samba 4.15.13 (samba-gplv3, Version wie Kodi 21.3), nur libsmbclient, statisch, für Kodis
# SMB-Quellen. Wie Kodis tools/depends/target/samba-gplv3: Patches 01-04 (die übrigen gelten nur
# für Android/Apple/webOS), gleiche configure-Optionen, "waf --targets=smbclient", alle internen
# Samba-Bibliotheken eingebaut (--builtin-libraries=...,ALL), sodass libsmbclient.a alles von
# Samba selbst enthält. Fremde Bibliotheken (gnutls -> nettle/hogweed/gmp, zlib, ...) nennt
# smbclient.pc in Libs.private/Requires.private (dafür sorgt 04-built-static.patch).
#
# Build-Rechner braucht: perl Parse::Yapp (pidl, Paket libparse-yapp-perl), flex + bison (das
# eingebettete Heimdal erzeugt daraus Parser) und die Heimdal-Werkzeuge asn1_compile/compile_et
# für den PC (03-builtin-heimdal.patch verlangt sie, denn beim Cross-Build kann Samba seine
# eigenen nicht ausführen). Diese baut das Skript wie Kodis tools/depends/native/heimdal
# (Heimdal 7.7.0 vom Kodi-Mirror) nach ~/flex/native/bin.
set -eu
. "$(dirname "$0")/../lib.sh"
is_done samba && exit 0
N=${FLEX_BUILD_ROOT:-$HOME/flex}/native

# --- Host-Pakete
miss=
perl -MParse::Yapp -e1 2>/dev/null || miss="$miss libparse-yapp-perl"
command -v flex >/dev/null || miss="$miss flex"
command -v bison >/dev/null || miss="$miss bison"
if [ -n "$miss" ]; then
  echo "Host-Pakete installieren:$miss"
  sudo -n apt-get install -y -q $miss
fi

# --- Heimdal-Werkzeuge für den PC (Host-Compiler, nicht die TV-Toolchain)
if [ ! -x "$N/bin/asn1_compile" ] || [ ! -x "$N/bin/compile_et" ]; then
  (
    unset CC CXX AR RANLIB STRIP NM CFLAGS CXXFLAGS CPPFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
    K=$KODI_SRC/tools/depends/native/heimdal
    eval "$(sed -n -E 's/\$\(([A-Z_]+)\)/${\1}/g; s/^(LIBNAME|VERSION|SOURCE|ARCHIVE|SHA512)=([^\\]*)$/\1="\2"/p' "$K/Makefile")"
    A=$DL/$ARCHIVE
    if [ ! -f "$A" ] || ! echo "$SHA512  $A" | sha512sum -c --status -; then
      curl -fsSL -o "$A.part" "$MIRROR/$ARCHIVE" && mv "$A.part" "$A"
    fi
    echo "$SHA512  $A" | sha512sum -c --status - || { echo "FEHLER: SHA512 von $ARCHIVE passt nicht"; exit 1; }
    H=$BUILD/heimdal-native
    rm -rf "$H" && mkdir -p "$H" && tar -xf "$A" -C "$H" --strip-components=1
    cd "$H"
    patch -p1 -i "$K/01-disable-libedit.patch"
    patch -p1 -i "$K/02-autoconf-2.70-configure.patch"
    # Ausgabe ins Log (build-deps.sh), damit ein Abbruch hier zu sehen ist
    autoreconf -vif
    # Heimdals rk_SYS_LARGEFILE liest ac_cv_sys_file_offset_bits/ac_cv_sys_large_files, die
    # Autoconf 2.72 nicht mehr setzt (Kodi nimmt 2.71) -> "-D_FILE_OFFSET_BITS=" ohne Wert, an dem
    # schon der size_t-Test scheitert. Auf dem 64-Bit-PC ist die richtige Antwort "no".
    # -std=gnu17: GCC 15 übersetzt sonst als C23, das der alte Heimdal-Code nicht ist.
    ./configure --prefix="$N" --disable-shared CFLAGS="-O2 -std=gnu17" \
      ac_cv_sys_file_offset_bits=no ac_cv_sys_large_files=no
    for d in include lib/roken lib/vers lib/com_err lib/asn1; do make -C "$d" -j"$JOBS"; done
    mkdir -p "$N/bin"
    install lib/asn1/asn1_compile lib/com_err/compile_et "$N/bin/"
    echo "heimdal $VERSION (PC): asn1_compile und compile_et in $N/bin"
  )
fi
export PATH=$N/bin:$PATH

# --- Samba
fetch samba-gplv3
cd "$S"
K=$KODI_SRC/tools/depends/target/samba-gplv3
for p in 01-fix-dependencies 02-cross_compile 03-builtin-heimdal 04-built-static; do
  patch -p1 -i "$K/$p.patch"
done
# Kodis pkg-config ist mit --enable-indirect-deps gebaut und nennt immer auch die privaten
# Abhängigkeiten. Samba prüft gnutls-Funktionen (z. B. gnutls_aead_cipher_encryptv2 für SMB3)
# per Link-Test gegen das statische libgnutls.a; nur mit "-lgnutls" (ohne nettle/hogweed/gmp/zlib)
# scheitern diese Tests und Samba schaltet die Funktionen still ab. Also hier pkg-config --static.
mkdir -p "$S/.flex-bin"
cat > "$S/.flex-bin/pkg-config" <<'EOF'
#!/bin/sh
exec /usr/bin/pkg-config --static "$@"
EOF
chmod 755 "$S/.flex-bin/pkg-config"
export PATH=$S/.flex-bin:$PATH
# -Wno-error=int-conversion wie bei Kodi (Clang >= 15). Clang >= 16 macht außerdem fehlende
# Prototypen zum Fehler: der configure-Test "setreuid is available" (übersetzt lib/util/setid.c
# ohne <grp.h>) scheitert dann an setgroups(), und ohne irgendeine setuid-Methode bricht
# configure ab ("setuidx is available" ist Pflicht). Ebenso implizites int: mehrere Tests sind im
# K&R-Stil ("main() {...}") geschrieben und scheitern sonst an der Form statt an der Funktion,
# darunter HAVE_MAKEDEV, das libsmbclient selbst nutzt (clifile.c). Im eigentlichen Build greift
# keine der Lockerungen.
export CFLAGS="$CFLAGS -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int"
# Ohne Antwortdatei: Samba legt cross-answers.txt selbst an, unbeantwortete Lauf-Tests zählen als
# "nein"; 02-cross_compile.patch macht die wichtigen davon zu reinen Übersetzungs-Tests.
./configure --prefix="$PREFIX" \
  --cross-compile --cross-answers=cross-answers.txt \
  --without-cluster-support --without-ldap --without-pam --enable-fhs --without-winbind \
  --without-ads --disable-avahi --disable-cups --without-gettext --without-ad-dc \
  --without-acl-support --without-utmp --disable-iprint --nopyc --nopyo \
  --builtin-libraries='!smbclient,!smbd_base,!smbstatus,ALL' \
  --with-shared-modules='!vfs_snapper' --disable-python --disable-symbol-versions \
  --without-json --without-libarchive --without-regedit --without-lttng
WAF_MAKE=1 ./buildtools/bin/waf -j"$JOBS" --targets=smbclient
WAF_MAKE=1 ./buildtools/bin/waf install --targets=smbclient
install -m644 bin/default/source3/libsmb/libsmbclient.a "$PREFIX/lib/"
done_stamp samba
