#!/bin/sh
# core-purity.sh -- prove src/core/ is still a standalone pure-C library.
#
# The layering rule (STDLIB_OOP_PLAN.md §2.1): a core TU knows nothing about the
# JS engine, so any module -- and the engine itself -- can link it. Two checks,
# because either alone is fooled:
#
#   1. grep for engine identifiers. Catches an #include that a header guard or a
#      dead #if would hide from the compiler.
#   1b. every quoted #include must be a core header or on ALLOWED_EXTERNAL_HEADERS.
#      Arm 2 needs -Isrc to resolve the allowlisted headers, and -Isrc would
#      otherwise let ANY header under src/ in silently. This keeps the core's
#      out-of-layer dependencies enumerated rather than merely reachable.
#   2. compile each core TU with only -Isrc/core plus those allowlisted headers.
#      Catches a real dependency that grep misses (a type pulled in
#      transitively, a macro from cutils.h).
#
# Runs in the 0-warning gate: -Wall -Wextra -Werror, same as the engine.
# Exit 0 = pure. Any violation prints the offending file and exits 1.
set -eu

CORE_DIR=src/core
CC=${CC:-cc}
rc=0

[ -d "$CORE_DIR" ] || { echo "core-purity: no $CORE_DIR yet -- nothing to check"; exit 0; }

# ---- 1. no engine identifiers -------------------------------------------------
# JSValue/JSContext/JSRuntime/JS_* and the engine's own headers. \b via -w is not
# portable across greps, so the pattern is explicit.
#
# COMMENTS ARE STRIPPED FIRST. A core file's own header comment says "no
# JSValue, no JSContext" -- grepping raw text would fail on the documentation
# that states the rule, and the obvious "fix" (rewording the comment) would make
# the check a trap for the next person. Only code is checked.
# The JS_ alternative is anchored on a non-identifier char: without that it
# matches inside DYNAJS_SIMD_KERNELS_H and flags an allowlisted pure-C header.
BANNED='JSValue|JSContext|JSRuntime|JSModuleDef|JSClassID|(^|[^A-Za-z0-9_])JS_[A-Za-z_]|dynajs\.h|dyna-nat\.h|cutils\.h'

# Non-core headers a core TU may include. Each must itself be free of the engine
# -- dyna-simd-kernels.h is the multi-ISA kernel table: stddef/stdint/stdbool/
# string/math/float/immintrin only, no JSValue anywhere. Keeping this an
# explicit list is the point: the core's dependencies are enumerated, not
# whatever happens to be reachable through -Isrc.
# dtoa.h: the engine's decimal<->double conversion, and it is genuinely
# standalone -- zero JSContext/JSValue/JSRuntime identifiers in dtoa.{c,h}, and
# dtoa.h includes nothing. A correctly-rounded, LOCALE-FREE float parse is not
# something a wire-protocol codec can hand-roll: the digit accumulator this
# replaced returned 1.0000000000000002e+300 for 1e300 against a real server.
ALLOWED_EXTERNAL_HEADERS='dyna-simd-kernels\.h|dtoa\.h'

# Blank out /*...*/ (including multi-line) and //... , preserving line numbers.
strip_comments() {
    awk '
    { line = $0; out = ""; i = 1; n = length(line)
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (blk) { if (d == "*/") { blk = 0; i += 2 } else i++ ; continue }
        if (d == "/*") { blk = 1; i += 2; continue }
        if (d == "//") break
        out = out c; i++
      }
      print out
    }' "$1"
}

for f in "$CORE_DIR"/*.c "$CORE_DIR"/*.h; do
    [ -e "$f" ] || continue
    if hits=$(strip_comments "$f" | grep -nE "$BANNED"); then
        echo "core-purity: FAIL $f references the engine:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi
    # Every quoted #include must be a core header or explicitly allowlisted.
    # Arm 2 compiles with -Isrc so the allowlisted header resolves; without this
    # check that -Isrc would also silently let in anything else under src/.
    incs=$(strip_comments "$f" | grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' || true)
    if [ -n "$incs" ]; then
        echo "$incs" | while IFS= read -r line; do
            hdr=$(printf '%s' "$line" | sed -E 's/.*"([^"]*)".*/\1/')
            [ -e "$CORE_DIR/$hdr" ] && continue
            printf '%s' "$hdr" | grep -qE "^($ALLOWED_EXTERNAL_HEADERS)$" && continue
            echo "core-purity: FAIL $f includes non-core header \"$hdr\" (line ${line%%:*})"
            echo "    add it to ALLOWED_EXTERNAL_HEADERS only if it is itself engine-free"
            exit 1
        done || rc=1
    fi
done

# ---- 2. each TU compiles standalone ------------------------------------------
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

for f in "$CORE_DIR"/*.c; do
    [ -e "$f" ] || continue
    if ! err=$("$CC" -c -std=gnu17 -Wall -Wextra -Werror -O2 \
                     -I"$CORE_DIR" -Isrc "$f" -o "$tmp/$(basename "$f").o" 2>&1); then
        echo "core-purity: FAIL $f does not build with -I$CORE_DIR (+ allowlisted headers):"
        echo "$err" | sed 's/^/    /'
        rc=1
    fi
done

# A HEADER-ONLY core (all static inline, e.g. dyn-bits.h) has no .c, so the loop
# above would never touch it and it would drift unchecked. Compile a synthesized
# TU per such header. -Wunused-function is dropped only here: an unused static
# inline in a header is the normal case, not a defect.
for h in "$CORE_DIR"/*.h; do
    [ -e "$h" ] || continue
    base=$(basename "$h" .h)
    [ -e "$CORE_DIR/$base.c" ] && continue
    printf '#include "%s.h"\n' "$base" > "$tmp/$base.probe.c"
    if ! err=$("$CC" -c -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function -O2 \
                     -I"$CORE_DIR" -Isrc "$tmp/$base.probe.c" -o "$tmp/$base.probe.o" 2>&1); then
        echo "core-purity: FAIL $h (header-only) does not build with -I$CORE_DIR (+ allowlisted headers):"
        echo "$err" | sed 's/^/    /'
        rc=1
    fi
done

[ "$rc" -eq 0 ] && echo "core-purity: OK ($(ls "$CORE_DIR"/*.c 2>/dev/null | wc -l | tr -d ' ') core TUs pure)"
exit "$rc"
