# Hilfsfunktionen für die Abhängigkeits-Skripte (gesourct).
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
MIRROR=https://mirrors.kodi.tv/build-deps/sources

# fetch <kodi-depends-ordner>: liest LIBNAME/VERSION/ARCHIVE/SHA512 aus Kodis
# tools/depends/target/<ordner>/*-VERSION, lädt vom Kodi-Mirror, prüft SHA512, entpackt nach
# $BUILD/<ordner> (frisch). Setzt $S (Quellverzeichnis) und $PKG_VERSION.
fetch() {
  local dir=$1 vf
  # Versionsangaben stehen in *-VERSION oder (ältere Pakete) direkt im Makefile
  vf=$(ls "$KODI_SRC/tools/depends/target/$dir/"*-VERSION 2>/dev/null | head -1)
  [ -n "$vf" ] || vf=$KODI_SRC/tools/depends/target/$dir/Makefile
  local LIBNAME VERSION ARCHIVE SHA512 BASE_URL SOURCE
  eval "$(sed -n -E 's/\$\(([A-Z_]+)\)/${\1}/g; s/^(LIBNAME|VERSION|SOURCE|ARCHIVE|SHA512|BASE_URL)=([^\\]*)$/\1="\2"/p' "$vf")"
  [ -n "${BASE_URL:-}" ] || BASE_URL=$MIRROR
  local f=$DL/$ARCHIVE
  if [ ! -f "$f" ] || ! echo "$SHA512  $f" | sha512sum -c --status -; then
    curl -fsSL -o "$f.part" "$BASE_URL/$ARCHIVE" && mv "$f.part" "$f"
  fi
  echo "$SHA512  $f" | sha512sum -c --status - || { echo "FEHLER: SHA512 von $ARCHIVE passt nicht"; return 1; }
  rm -rf "$BUILD/$dir"
  mkdir -p "$BUILD/$dir"
  tar -xf "$f" -C "$BUILD/$dir" --strip-components=1
  S=$BUILD/$dir
  PKG_VERSION=$VERSION
  echo "$dir $VERSION: $ARCHIVE (SHA512 ok)"
}

# done_stamp <name> / is_done <name>
is_done() { [ -f "$BUILD/.done-$1" ]; }
done_stamp() { touch "$BUILD/.done-$1"; }
