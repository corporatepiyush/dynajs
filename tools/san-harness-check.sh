#!/usr/bin/env bash
# tools/san-harness-check.sh -- prove the sanitizer harness is LIVE before the
# gate trusts it.
#
# B1-10's whole point is that a sanitizer stage which reports "clean" because
# nothing was instrumented is worthless. So the gate plants two defects in a
# throwaway program built with the SAME flags the sanitizer stage builds the
# engine with, and requires them to be reported:
#
#   1 a one-byte heap over-read  -> must be caught by AddressSanitizer
#   2 an out-of-bounds pointer   -> must be caught by UndefinedBehaviorSanitizer
#
# A leak is planted too, but LeakSanitizer is not available on every host
# (Darwin arm64 has no LSan at all), so its result is REPORTED, not required.
# The stage prints "san-harness: ..." so the gate log states exactly what the
# pass can and cannot see.
#
# Usage: tools/san-harness-check.sh <objdir-for-scratch>
# Exit:  0 the planted memory/UB defects were both reported
#        1 the harness is BLIND (fatal: a clean sanitizer run would be a lie)
#        2 no usable compiler (the caller decides whether that is fatal)
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

SCRATCH="${1:-$PWD/.obj/.dev/san-harness}"
mkdir -p "$SCRATCH" || exit 2
CC_BIN=""
if command -v clang >/dev/null 2>&1; then CC_BIN=clang
elif command -v gcc >/dev/null 2>&1; then CC_BIN=gcc
else echo "san-harness: SKIPPED -- no clang/gcc"; exit 2; fi

cat > "$SCRATCH/plant.c" <<'EOF'
#include <stdlib.h>
volatile int sink;
/* noinline: the object size of the result is unknown at the call site, so
   UBSan's object-size check cannot fire and ONLY ASan's shadow memory can
   catch the over-read. Without it UBSan reports first and the stage would
   "prove" an ASan capability it never exercised. */
__attribute__((noinline)) char *dyna_harness_alloc(unsigned n){ return (char*)malloc(n); }
/* defect 1: a heap over-read past the allocation (ASan) */
int overread(void){ char *p = dyna_harness_alloc(8); sink = p[16]; free(p); return sink; }
/* defect 2: signed integer overflow (UBSan) */
int overflow(void){ int x = 2147483647; volatile int y = x; return y + 1; }
/* defect 3: a leak (reported only where LeakSanitizer exists) */
int *leaked(void){ int *p = (int*)malloc(64); p[0] = 1; return p; }
int main(int argc, char **argv){
  if (argc > 1 && argv[1][0] == 'l') return leaked() != 0;
  if (argc > 1 && argv[1][0] == 'o') return overread();
  sink = overflow(); return 0;
}
EOF

CFLAGS="-g -O1 -fno-omit-frame-pointer -std=gnu17"
if ! $CC_BIN $CFLAGS -fsanitize=address -fsanitize=undefined "$SCRATCH/plant.c" -o "$SCRATCH/plant" 2>"$SCRATCH/cc.log"; then
  # some toolchains want the sanitizers comma-joined
  $CC_BIN $CFLAGS -fsanitize=address,undefined "$SCRATCH/plant.c" -o "$SCRATCH/plant" 2>>"$SCRATCH/cc.log" || {
    echo "san-harness: BLIND -- cannot compile an ASan+UBSan program with $CC_BIN"; tail -5 "$SCRATCH/cc.log"; exit 1; }
fi

LEAK_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1"

asan_seen=no; ubsan_seen=no; leak_seen=NOT-SUPPORTED
# Run each probe as a BACKGROUND job and wait for it: a non-interactive shell
# prints no "Abort trap" line for a job it reaps, and the sanitizer's own
# report lands in the log where it belongs.
run_probe(){  # $1 = log, rest = env assignments then the command
  local log="$1"; shift
  ( "$@" ) >"$log" 2>&1 &
  local pid=$!
  wait "$pid" 2>/dev/null
  return 0
}
AOPTS="ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1"
UOPTS="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
run_probe "$SCRATCH/o.log" env $AOPTS $UOPTS "$SCRATCH/plant" o
grep -q "ERROR: AddressSanitizer" "$SCRATCH/o.log" && asan_seen=yes

run_probe "$SCRATCH/u.log" env $AOPTS $UOPTS "$SCRATCH/plant"
grep -qE "runtime error:|SUMMARY: UndefinedBehaviorSanitizer" "$SCRATCH/u.log" && ubsan_seen=yes

run_probe "$SCRATCH/l.log" env $LEAK_ENV "$SCRATCH/plant" l
if grep -q "LeakSanitizer" "$SCRATCH/l.log"; then leak_seen=yes
elif grep -qi "not supported\|unsupported\|detect_leaks" "$SCRATCH/l.log"; then leak_seen="no (LeakSanitizer absent on this host)"
else leak_seen=no; fi

echo "  san-harness: planted heap over-read reported by ASan: $asan_seen"
echo "  san-harness: planted signed-overflow reported by UBSan: $ubsan_seen"
echo "  san-harness: planted leak reported by LSan: $leak_seen"
# record the answer so the gate can set ASAN_OPTIONS detect_leaks accordingly
case "$leak_seen" in
  yes) printf '1' > "$SCRATCH/leak-support" ;;
  *)   printf '0' > "$SCRATCH/leak-support" ;;
esac
if [ "$asan_seen" != yes ] || [ "$ubsan_seen" != yes ]; then
  echo "san-harness: BLIND -- the sanitizers did not report defects they MUST report." >&2
  echo "  asan log:"; tail -8 "$SCRATCH/o.log" | sed 's/^/    /'
  echo "  ubsan log:"; tail -8 "$SCRATCH/u.log" | sed 's/^/    /'
  exit 1
fi
echo "  san-harness: the sanitizer harness is live (both planted defects reported)"
exit 0