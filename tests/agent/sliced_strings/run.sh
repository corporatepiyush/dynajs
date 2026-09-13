#!/bin/sh
# runs every probe under $1 (engine); each probe is _h.js + probe concatenated
# so helpers stay shared while every file remains self-contained portable JS.
# NOTE: probes are 01-19 (20 included); 21_bjson has its own runner (engine-only)
# usage: sh run.sh /path/to/engine [node]
E=$1
KIND=${2:-engine}
d=$(cd "$(dirname "$0")" && pwd)
tmp="$d/.runner_tmp"
mkdir -p "$tmp"
pass=0; fail=0; skip=0
for f in "$d"/[0-9]*.js; do
  case "$(basename "$f")" in 21_*) continue;; esac
  b=$(basename "$f")
  cat "$d/_h.js" "$f" > "$tmp/$b"
  out=$(timeout 60 "$E" "$tmp/$b" 2>&1); rc=$?
  if [ "$KIND" = "node" ] && [ "$rc" != "0" ]; then
    echo "SKIP(node) $b: $(echo "$out" | head -1)"
    skip=$((skip+1)); continue
  fi
  if [ $rc -eq 0 ] && echo "$out" | grep -q "^PASS"; then
    echo "PASS $b"; pass=$((pass+1))
  else
    echo "FAIL $b rc=$rc: $(echo "$out" | head -3)"
    fail=$((fail+1))
  fi
done
rm -rf "$tmp"
echo "RESULT pass=$pass fail=$fail skip=$skip"
[ $fail -eq 0 ]
