#!/bin/sh
# tests/agent/_h/run.sh — unified runner for the converted assert-based suites.
#
# usage: run.sh <engine-binary> <suite-dir> [probe-name ...]
#   Runs every probe (.js/.mjs, top level, excluding _*.js helpers and files
#   listed in tests/agent/_h/exclude.tsv) under `timeout 30`. Each probe is
#   prepended with the shared harness _h/h.js (zero-import, both engines).
#   Per probe: stdout+stderr+rc -> <suite>/out/<engine-name>/<probe>.txt
#   Summary:   <suite>/out/summary.tsv  (probe, engine, rc, pass, fail, ok)
#
# Suite verdict: FAIL (exit nonzero) if any probe lacks a SUITE line, has
# FAIL>0 on its SUITE line, or exits nonzero.
#
# .mjs probes: node runs them as modules; dynajs gets -m.
# Engine binaries named dynajs* are invoked as-is; everything else is
# assumed to be node-like (plain invocation).

E="$1"; D="$2"; shift 2
[ -x "$E" ] || { echo "run.sh: engine '$E' not executable" >&2; exit 2; }
[ -d "$D" ] || { echo "run.sh: suite dir '$D' missing" >&2; exit 2; }

H="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$H/../../.." && pwd)"
ENGN="$(basename "$E")"
EXCL="$H/exclude.tsv"
OUT="$D/out/$ENGN"
SUM="$D/out/summary.tsv"

is_excluded() { # suite file
    awk -F'\t' -v s="$1" -v f="$2" '$1==s && $2==f { found=1 } END { exit !found }' "$EXCL" 2>/dev/null
}

mkdir -p "$OUT"
rm -f "$OUT"/*.txt
: > "$SUM"

TMP="$(mktemp -d "$D/.run_tmp.XXXXXX")" || exit 2
trap 'rm -rf "$TMP"' EXIT

probe_fail=0; probe_pass=0
for f in "$D"/*.js "$D"/*.mjs; do
    [ -e "$f" ] || continue
    b="$(basename "$f")"
    case "$b" in _*) continue;; esac
    if is_excluded "$(basename "$D")" "$b"; then
        echo "EXCL $b"
        continue
    fi
    if [ $# -gt 0 ]; then
        keep=0
        for want in "$@"; do [ "$b" = "$want" ] && keep=1; done
        [ $keep -eq 1 ] || continue
    fi
    ext="${b##*.}"
    tmp="$TMP/$b"
    if [ -f "$D/_h.js" ]; then
        # suite-local helper prelude (e.g. sliced_strings/_h.js) sits between
        # the shared harness and the probe
        cat "$H/h.js" "$D/_h.js" "$f" > "$tmp"
    else
        cat "$H/h.js" "$f" > "$tmp"
    fi
    extra=""
    if [ "$ext" = "mjs" ]; then
        case "$ENGN" in dynajs*) extra="-m";; esac
    fi
    txt="$OUT/$b.txt"
    timeout 30 "$E" $extra "$tmp" > "$txt" 2>&1
    rc=$?
    printf '\n__RC__=%s\n' "$rc" >> "$txt"
    pline="$(grep '^SUITE ' "$txt" | tail -1)"
    pass="$(printf '%s' "$pline" | sed -n 's/^SUITE .* PASS \([0-9][0-9]*\) FAIL [0-9][0-9]*$/\1/p')"
    fail="$(printf '%s' "$pline" | sed -n 's/^SUITE .* PASS [0-9][0-9]* FAIL \([0-9][0-9]*\)$/\1/p')"
    if [ -z "$pass" ]; then pass=0; fi
    if [ -z "$fail" ]; then fail=0; fi
    ok=1
    [ "$rc" -eq 0 ] || ok=0
    [ "$fail" -eq 0 ] || ok=0
    [ -n "$pline" ] || ok=0
    if [ $ok -eq 1 ]; then
        probe_pass=$((probe_pass+1)); echo "OK   $b (pass=$pass)"
    else
        probe_fail=$((probe_fail+1)); echo "FAIL $b (rc=$rc pass=$pass fail=$fail) -- $OUT/$b.txt"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$b" "$ENGN" "$rc" "$pass" "$fail" "$ok" >> "$SUM"
done

echo "RUNNER $ENGN $(basename "$D") probes_ok=$probe_pass probes_fail=$probe_fail"
[ $probe_fail -eq 0 ]
