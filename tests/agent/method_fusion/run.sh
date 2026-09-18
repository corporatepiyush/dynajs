#!/bin/sh
# Method-fusion lane test runner. Usage: sh tests/agent/method_fusion/run.sh
# Expects CWD = engine tree root (./dynajs available). Node is the oracle.
set -u
HERE=$(dirname "$0")
TMP="$HERE/.tmp"
mkdir -p "$TMP"
fail=0

echo "== equivalence: dynajs vs node =="
for f in "$HERE"/mf_arg_forms.js "$HERE"/mf_fallbacks.js "$HERE"/mf_receivers.js \
         "$HERE"/mf_edges.js "$HERE"/mf_tasks.js "$HERE"/mf_backtrace.js; do
  b=$(basename "$f" .js)
  cat "$HERE/mf_harness.js" "$f" > "$TMP/$b.combined.js" 2>/dev/null
  if [ "$b" = "mf_backtrace" ]; then
    # backtrace probe is self-contained (relative line assertions)
    cp "$f" "$TMP/$b.combined.js"
  fi
  timeout 60 ./dynajs "$TMP/$b.combined.js" > "$TMP/$b.dyna.out" 2>&1; drc=$?
  timeout 60 node "$TMP/$b.combined.js" > "$TMP/$b.node.out" 2>&1; nrc=$?
  if [ "$drc" != "$nrc" ]; then echo "RC-DIFF $b: dynajs=$drc node=$nrc"; fail=1; fi
  if ! cmp -s "$TMP/$b.dyna.out" "$TMP/$b.node.out"; then
    echo "OUT-DIFF $b:"; diff "$TMP/$b.dyna.out" "$TMP/$b.node.out" | head -10; fail=1
  fi
  # probe count from the harness line
  line=$(grep -E "^mf_" "$TMP/$b.dyna.out" | head -1)
  echo "  $b: $line (rc=$drc)"
done

echo "== qbc bytecode-cache round-trip =="
for body in mf_qbc_body mf_qbc_corrupt_body; do
  src="$HERE/$body.js"
  qbc="$src.qbc"
  rm -f "$qbc"
  # node oracle (fresh run)
  node "$src" > "$TMP/$body.node.out" 2>&1; nrc=$?
  # cold run: compile + write blob
  DYNAJS_BYTECODE_CACHE=1 timeout 60 ./dynajs "$src" > "$TMP/$body.cold.out" 2>&1; crc=$?
  if [ ! -f "$qbc" ]; then echo "NO-BLOB $body (cold rc=$crc)"; fail=1; continue; fi
  # warm run: read blob back through the fused-op fixup/validators
  DYNAJS_BYTECODE_CACHE=1 timeout 60 ./dynajs "$src" > "$TMP/$body.warm.out" 2>&1; wrc=$?
  if [ "$crc" != "$nrc" ] || [ "$wrc" != "$nrc" ]; then echo "RC-DIFF $body"; fail=1; fi
  cmp -s "$TMP/$body.cold.out" "$TMP/$body.node.out" || { echo "COLD-DIFF $body"; fail=1; }
  cmp -s "$TMP/$body.warm.out" "$TMP/$body.node.out" || { echo "WARM-DIFF $body"; fail=1; }
  echo "  $body: cold/warm/node identical (rc=$crc)"

  # corruption battery: each mutation must be rejected or degrade to a
  # correct recompile -- never crash with wrong output.
  i=0
  for off_desc in "form:36" "idx:37" "atom:30" "zero:0"; do
    name=${off_desc%%:*}; off=${off_desc##*:}
    cp "$qbc" "$TMP/$body.mut.qbc"
    printf '\377' | dd of="$TMP/$body.mut.qbc" bs=1 seek=$off count=1 conv=notrunc status=none
    DYNAJS_BYTECODE_CACHE=1 timeout 60 ./dynajs "$src" > "$TMP/$body.mut.out" 2>&1; mrc=$?
    mrc=$?
    if [ $mrc -ge 128 ] || [ $mrc -eq 139 ]; then
      echo "CRASH $body corruption=$name rc=$mrc"; fail=1
    elif ! cmp -s "$TMP/$body.mut.out" "$TMP/$body.node.out"; then
      # A rejected blob MUST degrade to a correct recompile: same output.
      # A blob that is accepted but changed could misbehave -> fail.
      # (Corrupting the header degrades to recompile; corrupting operands
      # inside a validated op is rejected by the reader -> recompile.)
      echo "WRONG-OUTPUT $body corruption=$name"; fail=1
    else
      echo "  corruption $name: rejected/degraded correctly (rc=$mrc)"
    fi
    i=$((i+1))
  done
  rm -f "$qbc"
done

if [ $fail -eq 0 ]; then echo "method_fusion: ALL PASS"; else echo "method_fusion: FAILURES"; fi
exit $fail
