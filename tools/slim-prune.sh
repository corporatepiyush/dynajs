#!/usr/bin/env bash
# tools/slim-prune.sh -- derive MINCOMPANION_<module> for the Makefile's `one`
# targets by pruning one companion object at a time until the LINK breaks or a
# MODTESTS run fails. The surviving set is proven, not guessed:
#
#   1. baseline: link `make one-<m>` with the full SLIMCOMPANION (must pass).
#   2. link-prune: remove each companion; a removal sticks only if the binary
#      still links. nat.o is deleted before every probe so the registration
#      defines are always recompiled to match exactly what is linked.
#   3. test-proof: run MODTESTS_<m> on the minimal binary; on failure put
#      pruned objects back (dyna:file first -- the usual culprit is a test
#      reading fixtures through it) until green, else report NOT PRUNABLE.
#
# Output per module: a `MINCOMPANION_<m>=...` line (table form, for the
# Makefile) and the evidence log .obj/slim/prune/<m>.log. Exit 0 only if every
# module ended link-clean AND test-green (modules with an empty MODTESTS list
# prove link-clean only, which is all they claim).
#
# Usage: tools/slim-prune.sh <module> [module...]
# Knobs: MAKE_CFG (default "CONFIG_NATIVE_MODULES=y CONFIG_TLS=y" -- must
#        match the on-disk object cache), DEV_JOBS (test fan-out, default 2),
#        PRUNE_TIMEOUT (per probe link, default 120s).
set -u
cd "$(dirname "$0")/.." || exit 2

MOD_CFG=${MAKE_CFG:-CONFIG_NATIVE_MODULES=y CONFIG_TLS=y}
TMO=${PRUNE_TIMEOUT:-120}
TJOBS=${DEV_JOBS:-2}
TIMEOUT_BIN=$(command -v timeout || command -v gtimeout || true)
bound(){ if [ -n "$TIMEOUT_BIN" ]; then "$TIMEOUT_BIN" "$@"; else "$@"; fi; }
[ -n "$TIMEOUT_BIN" ] || echo "note: no timeout(1) found -- probes unbounded" >&2

# link_probe M SET: build one-<M> with MINCOMPANION_<M>=$SET; rc 0 iff linked.
# Failed probes append the linker's undefined-symbol report to the module log
# -- a prune verdict without its failure evidence is not auditable.
link_probe(){
  local m="$1" set="$2" rc log=".obj/slim/prune/$m.log"
  rm -f ".obj/slim/one-$m/dyna-nat.o"
  bound "$TMO" make --no-print-directory $MOD_CFG \
    "MINCOMPANION_$m=$set" "one-$m" >".obj/slim/prune/$m.probe" 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then
    {
      echo "-- probe failed (rc=$rc), set:$set"
      grep -B1 -A3 "Undefined symbols" ".obj/slim/prune/$m.probe" | head -12
    } >>"$log"
  fi
  return "$rc"
}

run_modtests(){   # M BIN TESTS: rc 0 iff green (vacuous when TESTS empty)
  local m="$1" bin="$2" tests="$3"
  [ -n "$tests" ] || return 0
  DEV_JOBS="$TJOBS" DYNAJS="./$bin" \
    ./tools/run-tests-parallel.sh $tests >/dev/null 2>&1
}

# Test verdicts run beside this pruner's own links (and sibling agents'
# builds), so a single red can be a load flake, not a missing dependency.
# Asymmetric by design: REJECTing a prune needs red twice -- the retry
# serialized (DEV_JOBS=1), which removes the concurrency the flake rides on.
# ACCEPTing needs green once; the standing `one` gate re-proves the set after.
verdict_modtests(){   # M BIN TESTS -> rc 0 = green
  local m="$1" bin="$2" tests="$3" rc
  run_modtests "$m" "$bin" "$tests" && return 0
  [ -n "$tests" ] || return 1
  echo "    red once -- retry serialized (flake guard)" >&2
  DEV_JOBS=1 DYNAJS="./$bin" \
    ./tools/run-tests-parallel.sh $tests >/dev/null 2>&1
}

prune_one(){
  local m="$1" dir log v base kept removed tryobj cand bin tests rest green
  dir=".obj/slim/prune"; mkdir -p "$dir"
  log="$dir/$m.log"; : >"$log"
  v=$(make --no-print-directory $MOD_CFG slim-vars MOD="$m") \
    || { echo "$m: FAIL (slim-vars)"; return 1; }
  base=$(printf '%s\n' "$v" | sed -n 's/^SLIMCOMPANION=//p')
  bin=".obj/slim/dynajs-one-$m$(printf '%s\n' "$v" | sed -n 's/^EXE=//p')"
  tests=$(printf '%s\n' "$v" | sed -n 's/^MODTESTS=//p')
  [ -n "$base" ] || { echo "$m: FAIL (empty SLIMCOMPANION)"; return 1; }
  echo "== $m baseline:$base" | tee -a "$log"

  link_probe "$m" "$base" || { echo "$m: FAIL (baseline link, see $log)"; return 1; }
  verdict_modtests "$m" "$bin" "$tests" \
    || { echo "$m: FAIL (baseline MODTESTS, see $log)"; return 1; }
  echo "== baseline link+tests ok" | tee -a "$log"

  # -- link-prune --
  kept="$base"; removed=""
  for tryobj in $base; do
    cand=""
    for o in $kept; do [ "$o" = "$tryobj" ] || cand="$cand $o"; done
    if link_probe "$m" "$cand"; then
      kept="$cand"; removed="$removed $tryobj"
      echo "  pruned: $tryobj" | tee -a "$log"
    else
      echo "  needed: $tryobj" | tee -a "$log"
    fi
  done
  # fixpoint pass: one removal can free its dependents
  local moved=1
  while [ "$moved" = 1 ]; do
    moved=0
    for tryobj in $kept; do
      cand=""
      for o in $kept; do [ "$o" = "$tryobj" ] || cand="$cand $o"; done
      if link_probe "$m" "$cand"; then
        kept="$cand"; removed="$removed $tryobj"; moved=1
        echo "  pruned (fixpoint): $tryobj" | tee -a "$log"
      fi
    done
  done
  echo "== link-minimal:$kept" | tee -a "$log"

  # -- test-proof: re-add pruned objects (file first) until MODTESTS green --
  green=0
  if verdict_modtests "$m" "$bin" "$tests"; then
    green=1
  else
    echo "== tests red on link-minimal; re-adding" | tee -a "$log"
    rest=""
    for r in $removed; do
      if [ "$r" = ".obj/dyna-file.o" ]; then rest="$r $rest"; else rest="$rest $r"; fi
    done
    for r in $rest; do
      case " $kept " in *" $r "*) continue ;; esac
      if link_probe "$m" "$kept $r"; then
        kept="$kept $r"
        echo "  re-added: $r" | tee -a "$log"
        if verdict_modtests "$m" "$bin" "$tests"; then green=1; break; fi
      else
        echo "  re-add did not link: $r" | tee -a "$log"
      fi
    done
    if [ "$green" = 0 ]; then
      echo "== re-adds insufficient -- full SLIMCOMPANION retry" | tee -a "$log"
      link_probe "$m" "$base" && verdict_modtests "$m" "$bin" "$tests" && {
        green=1; kept="$base"
      }
    fi
  fi
  [ "$green" = 1 ] || {
    echo "== tests still red -> $m NOT PRUNABLE (full SLIMCOMPANION kept)" | tee -a "$log"
    return 1
  }
  echo "== final:$kept" | tee -a "$log"
  local line="MINCOMPANION_$m=$(printf '%s ' $kept)"
  echo "${line% }"
  echo "${line% }" >> "$dir/TABLE.txt"
}

rc=0
for m in "$@"; do
  prune_one "$m" || rc=1
done
exit "$rc"
