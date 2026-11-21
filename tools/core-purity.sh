#!/bin/sh
# core-purity.sh -- prove src/core/ is still a standalone pure-C library.
#
# The layering rule: a core TU knows nothing about the JS engine, so any
# module -- and the engine itself -- can link it. Two checks,
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

BANNED='JSValue|JSContext|JSRuntime|JSModuleDef|JSClassID|(^|[^A-Za-z0-9_])JS_[A-Za-z_]|dynajs\.h|dyna-nat\.h|cutils\.h'

ALLOWED_EXTERNAL_HEADERS='dtoa\.h|libunicode\.h|stb_truetype\.h|stb_textedit\.h'
EXTRA_INC=""
[ -d third_party/ui/stb ] && EXTRA_INC="-isystem third_party/ui/stb -isystem third_party/ui/fonts"

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

for f in "$CORE_DIR"/*.c "$CORE_DIR"/*.h "$CORE_DIR"/*/*.c "$CORE_DIR"/*/*.h; do
    [ -e "$f" ] || continue
    if hits=$(strip_comments "$f" | grep -nE "$BANNED"); then
        echo "core-purity: FAIL $f references the engine:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi
    incs=$(strip_comments "$f" | grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' || true)
    if [ -n "$incs" ]; then
        echo "$incs" | while IFS= read -r line; do
            hdr=$(printf '%s' "$line" | sed -E 's/.*"([^"]*)".*/\1/')
            [ -e "$CORE_DIR/$hdr" ] && continue
            [ -e "$(dirname "$f")/$hdr" ] && continue
            printf '%s' "$hdr" | grep -qE "^($ALLOWED_EXTERNAL_HEADERS)$" && continue
            echo "core-purity: FAIL $f includes non-core header \"$hdr\" (line ${line%%:*})"
            echo "    add it to ALLOWED_EXTERNAL_HEADERS only if it is itself engine-free"
            exit 1
        done || rc=1
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

NCPU=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
export CC CORE_DIR EXTRA_INC tmp
for h in "$CORE_DIR"/*.h "$CORE_DIR"/*/*.h; do
    [ -e "$h" ] || continue
    base=$(basename "$h" .h)
    [ -e "$(dirname "$h")/$base.c" ] && continue
    printf '#include "%s"\n' "${h#"$CORE_DIR"/}" > "$tmp/$base.probe.c"
done
{ for f in "$CORE_DIR"/*.c "$CORE_DIR"/*/*.c "$tmp"/*.probe.c; do
    [ -e "$f" ] || continue
    case "$f" in *.inc.c) continue ;; esac
    printf '%s\n' "$f"
  done; } | xargs -P "$NCPU" -I{} sh -c '
    f=$1; o="$tmp/$(echo "$f" | tr / _).o"; extra=""
    case "$f" in *.probe.c) extra=-Wno-unused-function ;; esac
    if ! err=$("$CC" -c -std=gnu17 -Wall -Wextra -Werror $extra -O2 -I"$CORE_DIR" -Isrc $EXTRA_INC "$f" -o "$o" 2>&1); then
        { echo "core-purity: FAIL $f does not build with -I$CORE_DIR (+ allowlisted headers):"; echo "$err" | sed "s/^/    /"; } > "$o.fail"
    fi' _ {}
for x in "$tmp"/*.fail; do [ -e "$x" ] || continue; cat "$x"; rc=1; done

[ "$rc" -eq 0 ] && echo "core-purity: OK ($(ls "$CORE_DIR"/*.c "$CORE_DIR"/*/*.c 2>/dev/null | grep -vc '\.inc\.c$' | tr -d ' ') core TUs pure)"
exit "$rc"
