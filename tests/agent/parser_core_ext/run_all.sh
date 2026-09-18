#!/bin/bash
# Runner for parser_core_ext: each .js runs on tree dynajs AND node v22;
# byte-identical stdout required unless the file is marked WHITELIST-DIVERGE.
# rt_* files additionally get a dynajsc compile -> embedded-bytecode binary run.
TREE="$(cd "$(dirname "$0")/../../.." && pwd)"
D="$TREE/tests/agent/parser_core_ext"
DYNA="$TREE/dynajs"
JSC="$TREE/dynajsc"
OUT="$D/out"
mkdir -p "$OUT"
pass=0; fail=0; wpass=0
: > "$OUT/summary.txt"
for f in "$D"/*.js; do
  b=$(basename "$f")
  wl=$(head -1 "$f" | grep -c "WHITELIST-DIVERGE" || true)
  "$DYNA" "$f" > "$OUT/$b.dyna" 2> "$OUT/$b.dyna.err"; rd=$?
  node "$f" > "$OUT/$b.node" 2> "$OUT/$b.node.err"; rn=$?
  if [ $rd -ne 0 ] || [ $rn -ne 0 ]; then
    echo "RC-FAIL $b dyna_rc=$rd node_rc=$rn" >> "$OUT/summary.txt"; fail=$((fail+1)); continue
  fi
  if cmp -s "$OUT/$b.dyna" "$OUT/$b.node"; then
    echo "PASS $b" >> "$OUT/summary.txt"; pass=$((pass+1))
  else
    if [ "$wl" -ge 1 ]; then
      echo "WLIST $b (outputs differ; whitelist: deep-tail PTC)" >> "$OUT/summary.txt"; wpass=$((wpass+1))
    else
      echo "FAIL $b" >> "$OUT/summary.txt"; fail=$((fail+1))
    fi
  fi
  case "$b" in
    rt*)
      rm -f "$OUT/$b.bin"
      "$JSC" -o "$OUT/$b.bin" "$f" > "$OUT/$b.jsc.log" 2>&1
      if [ $? -ne 0 ] || [ ! -x "$OUT/$b.bin" ]; then
        echo "JSC-FAIL $b" >> "$OUT/summary.txt"; fail=$((fail+1)); continue
      fi
      "$OUT/$b.bin" > "$OUT/$b.comp" 2>&1
      if cmp -s "$OUT/$b.dyna" "$OUT/$b.comp"; then
        echo "PASS $b [dynajsc round-trip]" >> "$OUT/summary.txt"
      else
        echo "FAIL $b [dynajsc round-trip]" >> "$OUT/summary.txt"; fail=$((fail+1))
      fi
      ;;
  esac
done
echo "---- pass=$pass wlist=$wpass fail=$fail ----" >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
