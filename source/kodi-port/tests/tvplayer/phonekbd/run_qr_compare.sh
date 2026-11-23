#!/bin/bash
# Auf der Build-VM: baut qr_test (Host) und qrref (Rust, offline) und vergleicht.
set -eu
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
HERE=$(cd "$(dirname "$0")" && pwd)
D=$HERE/../../../kodi-tree/xbmc/dialogs
OUT=${TMPDIR:-/tmp}/flexqr-test
mkdir -p "$OUT"
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$D" "$HERE/qr_test.cpp" "$D/FlexQr.cpp" -o "$OUT/qr_test"
( cd "$HERE/qrref" && CARGO_TARGET_DIR="$OUT/qrref-target" cargo build --offline --release -q )
python3 "$HERE/compare.py" "$OUT/qr_test" "$OUT/qrref-target/release/qrref" \
  "http://192.0.2.10:8781/k/0123456789abcdef01234567" \
  "http://192.0.2.123:8781/k/fedcba9876543210fedcba98" \
  "A" "HELLO WORLD" "https://kodi.tv/" "0123456789" \
  "$(printf 'x%.0s' $(seq 1 60))" "$(printf 'y%.0s' $(seq 1 120))" "$(printf 'z%.0s' $(seq 1 200))"
