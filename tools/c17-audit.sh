#!/bin/sh
# Compile every TU under strict -std=c17 as well as the -std=gnu17 the build
# pins. Flags come from the build engine (`./build.sh __cc-lines`) so this
# cannot drift from the real build: a hand-written recipe inherits none of the
# main build's defines.
#
# Strict ISO mode is not cosmetic. On glibc it hides POSIX declarations behind
# feature-test macros while the Darwin headers expose them regardless, so the
# same source compiles here and fails there. Run this in the glibc container
# too (tools/xplat-verify.sh), or it has only checked the forgiving platform.
#
# Usage: tools/c17-audit.sh [CONFIG_* args for __cc-lines]
set -eu

cd "$(dirname "$0")/.."
MAKEARGS="${*:-CONFIG_NATIVE_MODULES=y}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

rm -rf .obj.c17audit
trap 'rm -rf "$WORK" .obj.c17audit' EXIT
./build.sh __cc-lines $MAKEARGS 2>/dev/null > "$WORK/lines" || true

TOTAL=$(wc -l < "$WORK/lines" | tr -d ' ')
if [ "$TOTAL" -eq 0 ]; then
    echo "c17-audit: FAIL — no compile lines recovered from ./build.sh __cc-lines" >&2
    echo "  (did the config parse fail? try: ./build.sh __cc-lines CONFIG_NATIVE_MODULES=y)" >&2
    exit 1
fi

PASS=0; FAIL=0
: > "$WORK/failures"
while IFS= read -r line; do
    src=$(printf '%s\n' "$line" | sed -n 's/.* \([^ ]*\.c\)$/\1/p')
    [ -n "$src" ] || continue
    cmd=$(printf '%s\n' "$line" \
        | sed -e 's/-std=gnu17/-std=c17/' \
              -e 's/ -c -o [^ ]*\.o / -fsyntax-only /' \
              -e 's/-MMD -MF [^ ]*//g')
    if printf '%s\n' "$cmd" | sh - 2>"$WORK/err"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        printf '%s\n' "$src" >> "$WORK/failures"
        printf '\n--- %s ---\n' "$src" >> "$WORK/errlog"
        head -12 "$WORK/err" >> "$WORK/errlog"
    fi
done < "$WORK/lines"

echo "c17-audit: $TOTAL translation units, $PASS pass -std=c17, $FAIL fail"
if [ "$FAIL" -gt 0 ]; then
    echo "--- failing ---"
    cat "$WORK/failures"
    echo "--- diagnostics ---"
    cat "$WORK/errlog"
    exit 1
fi
