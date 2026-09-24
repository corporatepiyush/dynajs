#!/bin/zsh
# asan_sweep.sh -- run test files under the ASan+LSan dynajs and report,
# per file: rc + every leak stack's top frames.  Usage:
#   ./asan_sweep.sh <binary> <test.js> [test.js ...]
# Per-suite interpreter flags come from the runner's convention: the first
# 3 lines may declare `// flags: --std`.
DYN="$1"; shift
OUT="$(cd "$(dirname "$0")" && pwd)/sweep_out"
mkdir -p "$OUT"
for t in "$@"; do
  base="$(basename "$t" .js)"
  flags=$(head -3 "$t" | grep -E '^// *flags:' | head -1 | sed -E 's|^//[[:space:]]*flags:||')
  ASAN_OPTIONS=detect_leaks=1 timeout 600 "$DYN" ${=flags} "$t" > "$OUT/$base.out" 2> "$OUT/$base.err"
  rc=$?
  leaks=$(grep -c ' leak of ' "$OUT/$base.err")
  lsan=$(grep -c 'LeakSanitizer' "$OUT/$base.err")
  printf '%s rc=%s leaks=%s\n' "$base" "$rc" "$leaks"
  if [ "$leaks" -gt 0 ]; then
    # one line per distinct leak: size + the first app frame below the allocator
    awk '/ leak of /{sz=$0} /^    #[0-9]/{ if (sz!="" && ($0 ~ /in (df_|dyn_|js_)/)) { print "   " sz; print "   " $0; sz="" } }' "$OUT/$base.err" | sort -u
  fi
  if [ "$rc" -ne 0 ] && [ "$leaks" -eq 0 ]; then
    echo "   NONLEAK-FAILURE:"; tail -3 "$OUT/$base.out"; tail -3 "$OUT/$base.err"
  fi
done
