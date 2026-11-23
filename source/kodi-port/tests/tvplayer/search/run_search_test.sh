#!/bin/bash
# Builds and runs the host test of the tolerant library search (FlexFuzzySearch.cpp) on the build
# VM, with the host compiler and all warnings as errors.
# Extra arguments are passed on: run_search_test.sh <titles.tsv> "<query>" ranks a title list.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../../kodi-tree/xbmc/video
OUT=${TMPDIR:-/tmp}/flex_search_test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$SRC" -o "$OUT" "$HERE/search_test.cpp" "$SRC/FlexFuzzySearch.cpp"
"$OUT" "$@"
