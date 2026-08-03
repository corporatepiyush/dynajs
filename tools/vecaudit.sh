#!/usr/bin/env bash
# Per-function vectorization audit, ISA-aware. For each function in the generated
# assembly, count vector floating-point arithmetic against scalar. A hot loop with
# 0 vector ops is NOT vectorized, whatever the source comment claims.
#
#   tools/vecaudit.sh [src/dyna-ml.c]        # writes $ASM (default /tmp/vecaudit.s)
#   ASM=/tmp/x.s tools/vecaudit.sh src/foo.c
#
# Flags come from the BUILD SYSTEM, not from a list kept here. A hardcoded
# -DCONFIG_NATIVE_MODULE_* set compiles any module not on it down to an empty
# file: the audit then finds no vector ops, no scalar ops and no functions, and
# prints a clean report for code it never saw. That is indistinguishable from
# success, so the assembly is checked for function bodies before anything counts.
#
# Then run tools/libm-in-loops.sh over the same .s -- a libm call in a loop body
# makes that loop unvectorizable no matter how it is written, so that check is the
# decisive one. -Rpass-analysis=loop-vectorize says WHY a loop failed, but read
# bench/LOWLEVEL_PLAYBOOK.md first: those remarks count per inlined instance, so
# cold code inlined everywhere outranks the one hot loop.
set -uo pipefail
SRC="${1:-src/dyna-ml.c}"
ASM="${ASM:-/tmp/vecaudit-$(basename "${1:-dyna-ml}" .c).s}"   # per-source, or two runs clobber each other
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$ROOT" || exit 1
[ -f "$SRC" ] || { echo "vecaudit: no such file: $SRC" >&2; exit 2; }

# Ask make for the real recipe, so the audit sees what ships. The object is
# removed first or make reports nothing to do and hands back an empty command.
OBJ=".obj/$(basename "$SRC" .c).o"
rm -f "$OBJ" "${OBJ%.o}.d" "$OBJ.d" 2>/dev/null
CMD=$(make CONFIG_NATIVE_MODULES=y -n "$OBJ" 2>/dev/null | grep -m1 -- "$SRC")
[ -n "$CMD" ] || { echo "vecaudit: build system has no rule for $OBJ" >&2; exit 2; }

# -S in place of -c; -MMD would leave a dep file describing an object never built.
GEN=$(printf '%s' "$CMD" | sed "s| -c | -S |; s|-o $OBJ|-o $ASM|; s|-MMD||; s|-MF [^ ]*||")
eval "$GEN" 2>/dev/null || { echo "vecaudit: compile failed for $SRC" >&2; exit 1; }

# Refuse to report on an empty translation unit; everything below would otherwise
# print a clean audit of nothing at all.
NFUNC=$(LC_ALL=C grep -E '^_?[a-zA-Z_][a-zA-Z0-9_]*:' "$ASM" | grep -vcE '^\.?L')
if [ "$NFUNC" -lt 1 ]; then
    echo "vecaudit: $SRC produced no functions ($(wc -l < "$ASM") lines of asm)." >&2
    echo "vecaudit: it is compiled out under this configuration." >&2
    exit 1
fi

# Decided by register syntax, not by whether vector instructions are present: a
# fully scalar arm64 file has none and would read as x86. Mach-O arm64 carries no
# .arch directive either -- only `.build_version macos` -- so the triple is not
# available here. Load-pair is arm-only; %-prefixed registers are x86 AT&T.
if LC_ALL=C grep -qE '^[[:space:]]+(ldp|stp)[[:space:]]' "$ASM"; then ISA=arm
elif LC_ALL=C grep -qE '%r[a-z0-9]+' "$ASM"; then ISA=x86
else ISA=unknown; fi
[ "$ISA" = unknown ] && { echo "vecaudit: cannot identify the ISA of $ASM" >&2; exit 1; }
echo "vecaudit: $SRC -> $ASM   ($NFUNC functions, isa=$ISA)"

LC_ALL=C awk -v isa="$ISA" '
# clang writes the lane suffix ON the mnemonic: `fadd.2d v1, v1, v4`. A pattern
# expecting mnemonic and suffix as separate tokens matches nothing, and zero
# matches reads exactly like a scalar file. Register moves (movi.2d, mov.16b) are
# NOT arithmetic and must not be counted as vector work.
function is_vec(l) {
    if (isa == "arm")
        return (l ~ /\t(fadd|fsub|fmul|fdiv|fmla|fmls|fmax|fmin|fmaxnm|fminnm|fsqrt|fabs|fneg|faddp)\.(2d|4s)/)
    return (l ~ /[ \t]v?(add|sub|mul|div|max|min|sqrt)pd[ \t]/ ||
            l ~ /[ \t]vfm(add|sub)[0-9]*pd[ \t]/)
}
function is_scal(l) {
    if (isa == "arm")
        return (l ~ /\t(fadd|fsub|fmul|fdiv|fmadd|fmsub|fmax|fmin|fmaxnm|fminnm|fsqrt|fabs|fneg)\t[ds][0-9]/)
    return (l ~ /[ \t]v?(add|sub|mul|div|max|min|sqrt)sd[ \t]/ ||
            l ~ /[ \t]vfm(add|sub)[0-9]*sd[ \t]/)
}
# A FUNCTION label, not a basic-block label: Mach-O locals are Lxxx / Lloh,
# ELF locals are .Lxxx. Matching those would reset the counters mid-function and
# silently scatter one loop across a dozen fake "functions".
/^_?[a-zA-Z_][a-zA-Z0-9_]*:/ && $0 !~ /^\.?L/ {
    if (fn != "" && (vec + scal) > 0)
        printf "%-38s vec=%-6s scalarFP=%s\n", fn, vec, scal
    fn = substr($1, 1, length($1) - 1); vec = 0; scal = 0; next
}
fn != "" { if (is_vec($0)) vec++; else if (is_scal($0)) scal++ }
END { if (fn != "" && (vec + scal) > 0) printf "%-38s vec=%-6s scalarFP=%s\n", fn, vec, scal }
' "$ASM"
