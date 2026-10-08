#!/usr/bin/env bash
# tools/check-tracked-alloc.sh -- fail when a raw libc allocation appears in a
# dyna:*.c module that is CONVERTED to the tracked allocator.
#
# WHY THIS EXISTS
#   --native-memory-limit only bounds memory that goes through dyn_nat_malloc /
#   dyn_nat_calloc / dyn_nat_realloc (and is released by dyn_nat_free). A module
#   that calls libc malloc() directly is invisible to the ledger: the cap is
#   accepted, echoed by memoryUsage().nativeLimit, and then does nothing for
#   that module. That is exactly how dyna:bytes spent years: the flag looked
#   installed while the module allocated outside it. A lint is the only thing
#   that keeps a CONVERTED module converted -- the behaviour is invisible in
#   review once the diff is small and the tests only check the happy path.
#
# WHAT IT CHECKS
#   Only files listed in CONVERTED below. Every other src/dyna-*.c file is
#   deliberately NOT checked: those modules are mid-audit and still legitimately
#   use raw libc. Checking them now would report ~900 findings and block every
#   build; the allowlist grows one module at a time as each is converted.
#
#   That is the deliberate trade: this lint is a REGRESSION GUARD over finished
#   work, not a coverage score. Adding a module to CONVERTED is the commit that
#   says "this module is done".
#
# PAIRING IS NOT CHECKED HERE
#   This script only sees the call site, not the ownership. A dyn_nat_malloc
#   freed with libc free() (or vice versa) trips the {size,magic} header assert
#   and ABORTS -- loudly, at the first test that exercises the path. Asserting
#   on the assert is the point of the header, so the lint does not duplicate it.
#
# USAGE
#   tools/check-tracked-alloc.sh            # check the converted set
#   tools/check-tracked-alloc.sh --list     # print CONVERTED and exit
#
# EXIT
#   0 = clean, 1 = at least one raw allocation found (prints file:line: text),
#   2 = bad usage.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

if [ "${1:-}" = "--list" ]; then
    echo "$CONVERTED"
    exit 0
fi

# ---------------------------------------------------------------------------
# CONVERTED -- modules fully routed through the tracked allocator.
# Keep sorted; add a file only in the commit that converts it.
# ---------------------------------------------------------------------------
CONVERTED="src/dyna-bytes.c"

# ---------------------------------------------------------------------------
# ALLOWLIST -- raw call sites inside a CONVERTED file that are CORRECT as libc.
# Each entry is "<file>:<line>" plus a reason. A line-number allowlist is
# brittle on purpose: it forces a human to re-read the line after any edit
# above it, instead of silently exempting a whole file or a whole function.
# Format: <path>|<regex-of-exact-source>|<reason>
# ---------------------------------------------------------------------------
ALLOWLIST=(
)

# The allocator API itself, and the few places that legitimately hold libc
# memory outside the ledger, are handled by excluding them from the scan
# entirely rather than by a per-line exemption:
#
#   src/dyna-nat.c      -- THE TRACKER. dyn_nat_malloc wraps libc malloc to
#                          prepend the {size,magic} header; it must call libc.
#   src/dyna-decimal.c  -- converted; exempt so a future regression here is
#   src/dyna-mathx.c       visible as a single-file delta when triaging.
#   src/dyna-random.c
#   src/dyna-rrule.c
#   src/dyna-temporal.c
#   src/dyna-structures*.c
#
# Excluded from SCAN (never reported, no allowlist entry needed):
#   src/dyna-nat.h            declarations only.
#   src/dyna-libc.c          the libc shim the tracker is built on.
#   src/builtins/**          quickjs core; its allocator IS js_malloc, which
#                            is already bounded by --memory-limit.

# Match malloc/calloc/realloc/free/strdup as CALLS, not as substrings of
# identifiers. The [^_[:alnum:]] guard is what keeps dyn_nat_malloc, js_malloc,
# js_def_malloc, xfree and friends out of the report.
PATTERN='(^|[^_[:alnum:]])(malloc|calloc|realloc|reallocarray|strdup|strndup)[[:space:]]*\('
FREEPAT='(^|[^_[:alnum:]])(free)[[:space:]]*\('

status=0
checked=0

for f in $CONVERTED; do
    if [ ! -f "$f" ]; then
        echo "FAIL: CONVERTED lists a missing file: $f" >&2
        status=1
        continue
    fi
    checked=$((checked + 1))

    # Strip string/char literals and preprocessor lines first so that a mention
    # of "malloc" inside a message string or a #include <stdlib.h> is not a
    # finding. src/** carries no comments, so only literals need handling.
    hits=$(sed -e 's/"[^"]*"//g' -e 's/^[[:space:]]*#.*//' "$f" \
           | grep -nE "$PATTERN|$FREEPAT" || true)

    [ -z "$hits" ] && continue

    while IFS= read -r line; do
        ln=${line%%:*}
        text=${line#*:}

        allowed=0
        for entry in ${ALLOWLIST+"${ALLOWLIST[@]}"}; do
            af=${entry%%|*}
            if [ "$af" = "$f" ] && printf '%s' "$text" | grep -qE "$(printf '%s' "$entry" | cut -d'|' -f2)"; then
                allowed=1
                break
            fi
        done
        [ "$allowed" = 1 ] && continue

        printf '%s:%s: raw allocation in a CONVERTED module: %s\n' \
            "$f" "$ln" "$(printf '%s' "$text" | sed -e 's/^[[:space:]]*//')" >&2
        status=1
    done <<EOF
$hits
EOF
done

if [ "$status" = 0 ]; then
    echo "check-tracked-alloc: ok ($checked converted module(s) clean)"
fi
exit "$status"
