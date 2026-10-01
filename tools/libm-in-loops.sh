#!/usr/bin/env bash
# Companion to tools/vecaudit.sh (which writes the .s file this reads).
# For every function in the assembly, count calls to libm transcendentals. A
# libm call inside a loop body means that loop CANNOT be vectorized, regardless
# of what the source looks like.
#
# THE DEFAULT PATH IS DERIVED THE SAME WAY vecaudit.sh DERIVES IT. It used to be
# a hardcoded /tmp/vecaudit.s; when vecaudit.sh moved to a per-source name so two
# runs could not clobber each other, this file was not updated and every run has
# failed with "No such file" since -- which took the whole amd64 image build down
# with it, so nothing was verified on x86-64 at all.
set -uo pipefail
SRC="${1:-src/dyna-ml.c}"
ASM="${ASM:-/tmp/vecaudit-$(basename "$SRC" .c).s}"
[ -r "$ASM" ] || { echo "libm-in-loops: $ASM not found -- run tools/vecaudit.sh $SRC first" >&2; exit 1; }
awk '
/^_[a-zA-Z_][a-zA-Z0-9_]*:/ {
    if (fn != "" && (l+e+p+s) > 0) printf "%-30s log=%-4s exp=%-4s pow=%-4s sqrt=%s\n", fn, l, e, p, s
    fn = substr($1, 1, length($1)-1); l=0; e=0; p=0; s=0; next
}
fn != "" && /[ \t]bl[ \t]/ {
    if ($0 ~ /_log$/)  l++
    if ($0 ~ /_exp$/)  e++
    if ($0 ~ /_pow$/)  p++
    if ($0 ~ /_sqrt$/) s++
}
END { if (fn != "" && (l+e+p+s) > 0) printf "%-30s log=%-4s exp=%-4s pow=%-4s sqrt=%s\n", fn, l, e, p, s }
' "$ASM"
