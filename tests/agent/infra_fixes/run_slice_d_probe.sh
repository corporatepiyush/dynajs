#!/bin/bash
OLD=${1:-./dynajs.pristine}
NEW=${2:-./dynajs}
HERE="$(cd "$(dirname "$0")" && pwd)"

for eng in "$OLD" "$NEW"; do
  echo "== $(basename "$eng") =="
  prev=""
  for N in 100 101 102 103 200; do
    line=$(DYNA_SLICE_MIN_LEN=32 "$eng" -d "$HERE/probe_slice_d.js" $N 2>/dev/null | grep '^strings')
    size=$(echo "$line" | awk '{print $3}')
    delta=""
    [ -n "$prev" ] && delta=$((size - prev))
    echo "  N=$N  strings_size=$size  delta_from_prev=${delta:-NA}"
    prev=$size
  done
done
