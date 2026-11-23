#!/bin/bash
# curl 8.10.0 mit OpenSSL, zlib und nghttp2 (HTTP/2), nur die Protokolle, die Kodi braucht
# (HTTP/HTTPS/FTP/FILE).
set -eu
. "$(dirname "$0")/../lib.sh"
is_done curl && exit 0
fetch curl
cd "$S"
./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
  --with-openssl="$PREFIX" --with-zlib="$PREFIX" --with-ca-fallback --without-ca-bundle --without-ca-path \
  --without-libpsl --without-brotli --without-zstd --with-nghttp2="$PREFIX" --without-libidn2 --without-librtmp \
  --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
  --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
  --disable-manual --disable-docs --disable-ntlm-wb
make -j"$JOBS" -C lib
make -C lib install
make -C include install
make install-pkgconfigDATA
done_stamp curl
