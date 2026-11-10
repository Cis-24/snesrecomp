#!/usr/bin/env bash
# The trace build's debug server keeps a frame history of
# SNESRECOMP_FRAME_HISTORY_SIZE records (default 6000) in static storage, each
# with a copy of WRAM and VRAM (at least 192 KiB). At 6000 frames that alone is
# 1.2 GB; with the trace rings a Windows trace build passed the 2 GB image limit
# and no longer started ("not a valid Win32 application"). The size has to be
# configurable, so a game can keep its trace build loadable.
#
# Compiles debug_server.c as a trace object twice, at the default and at 16
# frames, and requires the static data to shrink by at least the WRAM and VRAM
# copies of the dropped frames.
# Usage: frame_history_size_test.sh <snesrecomp root> <out dir>   (env: CC, SIZE)
set -euo pipefail
ROOT=${1:?snesrecomp root}
OUT=${2:?out dir}
CC=${CC:-gcc}
SIZE=${SIZE:-size}
mkdir -p "$OUT"

bss_of() { # frames ("" = default)
  local def=() obj="$OUT/debug_server_history_${1:-default}.o"
  [ -n "$1" ] && def=(-DSNESRECOMP_FRAME_HISTORY_SIZE="$1")
  "$CC" -std=c11 -O1 -w -c -DSNESRECOMP_TRACE=1 -DSNESRECOMP_REVERSE_DEBUG=0 ${def[@]+"${def[@]}"} \
      -I "$ROOT/runner/src" -I "$ROOT/runner/src/snes" \
      "$ROOT/runner/src/debug_server.c" -o "$obj"
  # Berkeley format: text data bss dec hex filename.
  "$SIZE" "$obj" | awk 'NR == 2 { print $2 + $3 }'
}

full=$(bss_of "")
small=$(bss_of 16)
need=$(( (6000 - 16) * (0x20000 + 0x10000) ))
echo "static data: default $full, 16 frames $small, difference $((full - small)), need >= $need"
if [ $((full - small)) -ge "$need" ]; then
  echo "frame_history_size_test: PASS"
else
  echo "FAIL: SNESRECOMP_FRAME_HISTORY_SIZE does not size the frame history"
  exit 1
fi
