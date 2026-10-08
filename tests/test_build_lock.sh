#!/usr/bin/env bash
# tests/test_build_lock.sh -- the per-tree build lock must be OWNED, BOUNDED and
# STALE-DETECTING (B1-07). Before the fix the lock was a fixed, world-shared
# /tmp/dyna_build.lock directory with no owner record, no timeout and no stale
# detection: a lock left by a SIGKILLed build blocked every later build forever,
# with no output, in every checkout on the host.
#
# Three properties, each proven here:
#   1 CONTENTION  two builds in ONE objdir serialize; the second says it is
#                 waiting and names the lock, then completes (rc 0).
#   2 STALE       a lock whose owner pid is gone is reclaimed, loudly.
#   3 BOUNDED     a lock that IS alive is waited on, and DEV_LOCK_TIMEOUT makes
#                 the wait FAIL naming the lock and its owner instead of hanging.
#   4 ISOLATION   two builds in DIFFERENT objdirs do not serialize at all.
#
# Every build here uses a private --out so the gate's own .obj is untouched.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
ROOT=$PWD
OUT_BASE=${OUT:-.obj-locktest}
SCRATCH="$ROOT/.obj-locktest-scratch"
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
pass=0; fail=0
ok(){ echo "  ok  $1"; pass=$((pass+1)); }
no(){ echo "FAIL: $1"; fail=$((fail+1)); }

DEV_LOCK_DIR="$SCRATCH/locks"
export DEV_LOCK_DIR
key="$(printf '%s' "$OUT_BASE" | tr -c 'A-Za-z0-9._-' '_').lock"   # same key _eng_lock_key builds

echo "== 1 CONTENTION: a second build waits for the lock, names it, then succeeds"
mkdir -p "$DEV_LOCK_DIR/$key"
# a live owner: this very shell's pid, so staleness detection must NOT fire
printf 'pid=%s\nhost=%s\nstart=%s\ncmd=fake-long-build\nobjdir=%s\n' \
  "$$" "$(hostname 2>/dev/null || echo unknown)" "$(date +%s)" "$OUT_BASE" \
  > "$DEV_LOCK_DIR/$key/owner"
# release the lock from a background sleeper after ~4s (simulates the holder finishing)
( sleep 4; rm -rf "$DEV_LOCK_DIR/$key" ) &
releaser=$!
t0=$(date +%s)
DEV_LOCK_TIMEOUT=60 DEV_STATE="$SCRATCH/state" ./build.sh --out "$OUT_BASE" build CONFIG_NATIVE_MODULES= >"$SCRATCH/contend.log" 2>&1
rc=$?
t1=$(date +%s)
wait $releaser 2>/dev/null
if [ "$rc" = 0 ]; then ok "contending build completed after waiting $((t1-t0))s"
else no "contending build failed rc=$rc (expected it to wait, then build)"; tail -5 "$SCRATCH/contend.log"; fi
grep -q "waiting .*s for the build lock" "$SCRATCH/contend.log" \
  && ok "the wait is reported, not silent" \
  || no "the wait printed nothing (a silent unbounded wait is the B1-07 failure mode)"
grep -q "held by: .*fake-long-build" "$SCRATCH/contend.log" \
  && ok "the wait names the lock owner" \
  || no "the wait did not name the lock owner"

echo "== 2 STALE: a lock whose owner pid is gone is reclaimed"
mkdir -p "$DEV_LOCK_DIR/$key"
printf 'pid=999999\nhost=%s\nstart=%s\ncmd=long-dead-build\nobjdir=%s\n' \
  "$(hostname 2>/dev/null || echo unknown)" "$(date +%s)" "$OUT_BASE" > "$DEV_LOCK_DIR/$key/owner"
DEV_LOCK_TIMEOUT=60 DEV_STATE="$SCRATCH/state" ./build.sh --out "$OUT_BASE" build CONFIG_NATIVE_MODULES= >"$SCRATCH/stale.log" 2>&1
rc=$?
[ "$rc" = 0 ] || { no "stale-lock build failed rc=$rc"; tail -5 "$SCRATCH/stale.log"; }
grep -q "reclaiming stale build lock .* (owner pid 999999 is gone)" "$SCRATCH/stale.log" \
  && ok "a dead owner's lock is reclaimed, loudly" \
  || { no "no stale-lock reclaim message"; tail -5 "$SCRATCH/stale.log"; }

echo "== 3 BOUNDED: a live lock times out FAILING, naming lock + owner (never hangs)"
mkdir -p "$DEV_LOCK_DIR/$key"
printf 'pid=%s\nhost=%s\nstart=%s\ncmd=wedged-build\nobjdir=%s\n' \
  "$$" "$(hostname 2>/dev/null || echo unknown)" "$(date +%s)" "$OUT_BASE" > "$DEV_LOCK_DIR/$key/owner"
t0=$(date +%s)
DEV_LOCK_TIMEOUT=5 DEV_STATE="$SCRATCH/state" ./build.sh --out "$OUT_BASE" build CONFIG_NATIVE_MODULES= >"$SCRATCH/timeout.log" 2>&1
rc=$?
t1=$(date +%s)
rm -rf "$DEV_LOCK_DIR/$key"
[ "$rc" != 0 ] && ok "the build FAILED on the wedged lock (rc=$rc)" || no "a wedged lock did not fail the build"
[ $((t1-t0)) -le 30 ] && ok "it failed in $((t1-t0))s, bounded (DEV_LOCK_TIMEOUT=5)" \
  || no "the wait was unbounded ($((t1-t0))s for a 5s timeout)"
grep -q "FAIL: build lock .* held for .* by: .*wedged-build" "$SCRATCH/timeout.log" \
  && ok "the failure names the lock and its owner" \
  || { no "the failure did not name the lock/owner"; tail -6 "$SCRATCH/timeout.log"; }

echo "== 4 ISOLATION: two builds in different objdirs never serialize"
rm -rf "$DEV_LOCK_DIR"; mkdir -p "$DEV_LOCK_DIR"
mkdir -p "$DEV_LOCK_DIR/$(printf '%s' ".obj-lockA" | tr -c 'A-Za-z0-9._-' '_').lock"
printf 'pid=%s\nhost=%s\nstart=%s\ncmd=other-tree-build\nobjdir=.obj-lockA\n' \
  "$$" "$(hostname 2>/dev/null || echo unknown)" "$(date +%s)" > "$DEV_LOCK_DIR/$(printf '%s' ".obj-lockA" | tr -c 'A-Za-z0-9._-' '_').lock/owner"
DEV_LOCK_TIMEOUT=5 DEV_STATE="$SCRATCH/state" ./build.sh --out "$OUT_BASE" build CONFIG_NATIVE_MODULES= >"$SCRATCH/iso.log" 2>&1
rc=$?
rm -rf "$DEV_LOCK_DIR"
[ "$rc" = 0 ] && ok "a lock in ANOTHER objdir does not block this build" \
  || { no "an unrelated objdir's lock blocked this build (rc=$rc)"; tail -5 "$SCRATCH/iso.log"; }

rm -rf "$ROOT/$OUT_BASE" "$SCRATCH"
echo "test_build_lock: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0