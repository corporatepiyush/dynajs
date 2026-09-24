#!/bin/bash
# run_one.sh PROBE.js ENGINE BIN OUTDIR TIMEOUT
P="$1"; ENG="$2"; BIN="$3"; OUT="$4"; TMO="${5:-60}"
b=$(basename "$P" .js)
timeout "$TMO" "$BIN" "$P" > "$OUT/$b.txt" 2>"$OUT/$b.stderr"
echo "$?" > "$OUT/$b.rc"
