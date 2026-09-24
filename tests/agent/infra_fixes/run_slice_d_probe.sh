#!/bin/bash
# run_slice_d_probe.sh -- INFRA 4 acceptance: the -d strings row for a shape
# holding one flat 1 MB parent + N live slices must be sane.
#
#   OLD model: each slice charged sizeof(JSString)+window as if it owned its
#   window; the parent's cost was diluted across a refcount it was visited
#   once against -- a 1 MB parent reported as ~0, totals ~6.8 KB.
#   NEW model: a slice is charged its 28-byte prefix block (16 payload + 12
#   header) and the parent's shared cost is charged once per holding
#   reference, so the parent counts as ~1 MB and every extra slice adds
#   exactly its 28-byte block (+ a sub-0.1% walker dilution residual the
#   file's own "poor man's approach" XXX documents).
#
# Usage: run_slice_d_probe.sh <dynajs-old> <dynajs-new>
# The 50-char windows sit under the default slice threshold (64), so the
# documented DYNA_SLICE_MIN_LEN override forces slicing for the probe.
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
