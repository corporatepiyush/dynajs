#!/usr/bin/env bash
# Run every ```js block in the API reference and report which ones fail.
#
# The blocks there are FRAGMENTS: each module section states its own
# `import { ... } from "dyna:x";` once, and the examples below it use those names
# bare. Executed as written they all die on ReferenceError, which is why nothing
# had ever run them -- and why six of them documented functions that no longer
# exist. So this prepends the nearest preceding import line from the same
# document before running the block, which is exactly what a reader does.
#
# A block that genuinely cannot run standalone -- one that needs a server, a
# path on disk, or a `...` elision -- carries `<!-- check:skip -->` on the line
# before its fence. Skips are COUNTED and printed, because a skip that prints
# nothing is how a suite quietly stops testing what it claims.
#
#   tools/check-api-examples.sh [FILE.md] [path/to/dynajs]
set -euo pipefail

SRC="${1:-API.md}"
BIN="${2:-./dynajs}"
[ -r "$SRC" ] || { echo "check-api-examples: cannot read $SRC" >&2; exit 1; }
[ -x "$BIN" ] || { echo "check-api-examples: cannot run $BIN" >&2; exit 1; }

OUT="${TMPDIR:-/tmp}/api-examples"
rm -rf "$OUT"; mkdir -p "$OUT"

# Which dyna:* modules this BUILD actually has. The names come from src/,
# not from the doc: API.md does not spell every module in a quoted form, and
# scanning it silently dropped dyna:scrape, so its examples failed as undefined.
# Probing once beats guessing:
# a static import of an absent module (dyna:uring is Linux-only) is fatal and
# would fail every example for a reason that is not doc rot.
DYNA_MODS=$(for m in $(grep -rhoE '"dyna:[a-z0-9]+"' src/ | tr -d '"' | sort -u); do
    "$BIN" -e "import(\"$m\").then(()=>print(\"$m\"))" 2>/dev/null
done | tr '\n' ' ')
export DYNA_MODS

python3 - "$SRC" "$OUT" <<'PY'
import re, sys, os
src, out = sys.argv[1], sys.argv[2]
lines = open(src).read().split("\n")

imp = None        # the most recent `import ... from "dyna:x";` seen in prose
n = 0
i = 0
ALL_MODULES = os.environ.get("DYNA_MODS", "").split()

while i < len(lines):
    ln = lines[i]
    # A section states its module in one of three shapes: a one-line backticked
    # import, one wrapped over several lines, or a fenced js block (# ml). Match
    # the MODULE NAME wherever it appears, not the import's exact spelling.
    if ln.startswith("`import ") or ln.lstrip().startswith("import {"):
        m2 = re.search(r'"(dyna:[a-z0-9]+)"', "\n".join(lines[i:i + 40]))
        if m2:
            imp = m2.group(1)
    if re.match(r"^# [a-z]", ln):
        imp = None                        # a new module section; forget the last
    if ln.strip() == "```js":
        skip = i > 0 and "check:skip" in lines[i - 1]
        body, j = [], i + 1
        while j < len(lines) and lines[j].strip() != "```":
            body.append(lines[j]); j += 1
        # A section may state its module INSIDE the first fenced block (# ml
        # does), and that block is consumed here before the scanner above sees
        # it -- so pick the module up from the body too.
        if imp is None:
            m3 = re.search(r'from "(dyna:[a-z0-9]+)"', "\n".join(body))
            if m3:
                imp = m3.group(1)
        n += 1
        name = "ex%03d" % n
        # Expose the whole module rather than replaying the doc's import line:
        # those lists are elided (`/* read*, write* */`) and abbreviated, so a
        # name the section documents but does not list would read as a doc bug.
        own = any(b.lstrip().startswith("import ") for b in body)
        head = ""
        if not own:
            # The inferred module is a GUESS from the nearest prose import, and
            # the last sections of API.md have none -- they inherited whatever
            # module was seen last (dataframe) and their TCPServer/AESGCM/
            # Fetcher examples failed as undefined, which reads as doc rot and
            # is not. Load the inferred module LAST so it still wins on a name
            # two modules share, but load the others first so an example is
            # never undefined merely because the guess was wrong.
            mods = [m for m in ALL_MODULES if m != imp] + ([imp] if imp else [])
            # Static imports over the modules this build actually has (the
            # shell probed them into DYNA_MODS). A static import of an absent
            # module is fatal; `await import()` is a syntax error here because
            # examples run as scripts, not modules.
            head = "".join('import * as __ns%d from "%s"; '
                           "Object.assign(globalThis, __ns%d);" % (k, m, k)
                           for k, m in enumerate(mods))
        with open(os.path.join(out, name + ".js"), "w") as f:
            if head: f.write(head + "\n")
            f.write("\n".join(body) + "\n")
        with open(os.path.join(out, name + ".meta"), "w") as f:
            f.write(("skip" if skip else "run") + "\n%d\n" % (i + 1))
        i = j + 1
        continue
    i += 1
print(n)
PY

# Two bounds, because the two ways an example hangs need different instruments:
# a spin burns CPU and trips `ulimit -t`; a block on a socket burns none and only
# wall clock catches it. A timeout is reported DISTINCTLY from a failure -- "it
# never finished" and "it returned the wrong answer" lead different places.
WALL_LIMIT=${WALL_LIMIT:-10}
CPU_LIMIT=${CPU_LIMIT:-10}
# `set -e` is a SHELL-WIDE setting, so restoring it inside the callee undoes the
# caller's `set +e` and the first failing example aborts the run -- which reads
# as "no examples failed". Never toggle it here; use `|| rc=$?`, which is exempt.
run_example() {
    local rc=0
    ( ulimit -t "$CPU_LIMIT" 2>/dev/null; exec "$BIN" "$1" ) > "$2" 2>&1 &
    epid=$!
    ( sleep "$WALL_LIMIT"; kill -9 "$epid" 2>/dev/null ) > /dev/null 2>&1 &
    wpid=$!
    wait "$epid" 2>/dev/null || rc=$?
    kill "$wpid" 2>/dev/null || true
    wait "$wpid" 2>/dev/null || true
    [ "$rc" -gt 128 ] && rc=124
    return "$rc"
}

total=0; failed=0; skipped=0; timedout=0
for js in "$OUT"/ex*.js; do
    name="$(basename "$js" .js)"
    total=$((total + 1))
    read -r mode < "$OUT/$name.meta"
    line="$(sed -n 2p "$OUT/$name.meta")"
    if [ "$mode" = "skip" ]; then
        skipped=$((skipped + 1))
        echo "  skip $name  ($SRC:$line)"
        continue
    fi
    rc=0; run_example "$js" "$OUT/$name.out" || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  ok   $name"
    elif [ "$rc" -eq 124 ]; then
        failed=$((failed + 1)); timedout=$((timedout + 1))
        echo "TIMEOUT $name  ($SRC:$line, killed after ${WALL_LIMIT}s)"
        sed 's/^/       /' "$js"
    else
        failed=$((failed + 1))
        echo "FAIL $name  ($SRC:$line)"
        head -4 "$OUT/$name.out" | sed 's/^/       /'
        echo "     --- source ---"
        sed 's/^/       /' "$js"
    fi
done

echo
echo "$SRC: $total examples, $failed failed ($timedout of them timeouts), $skipped skipped (check:skip)"
[ "$failed" -eq 0 ]
