#!/bin/sh
# tools/affected-modules.sh -- map changed files to test-module names.
#
# Running the full build/test matrix for a change that touched one or a few
# modules is forbidden waste. This script is the single derivation every
# scoped runner shares (`test-native MODULES=...`, ./build.sh test|gate): it
# turns a git diff into module names; the callers turn module names into suite
# filters (test_<m>*.js / bb_<m>.js substrings).
#
# Usage:
#   tools/affected-modules.sh [BASE]
#     BASE defaults to HEAD (the working tree vs HEAD); any rev or range the
#     caller wants is accepted ("origin/master..HEAD" -- choosing the range is
#     the caller's job). Untracked files under src/ and tests/ count as
#     changed too: new module work usually starts life untracked. Untracked
#     junk anywhere else does not force the matrix on anyone.
#
# Output: module names, space-separated, deduped, sorted -- `core` FIRST when
# present (callers treat core as "run everything"). `infra` is printed as-is
# for gate/build-machinery changes; callers decide what infra means. Empty
# output means no changed file mapped to anything.
#
# Mapping rules (a module name is a dyna:<m> suite family, i.e. exactly the
# test_<m>*.js / bb_<m>.js substring the scoped runners filter on):
#   src/dyna-<m>.c|.inc.c|.h  -> <m>   special cases first:
#                                        dyna-simd* -> simd (simd is a module)
#                                        dyna-libc.* -> "std os"
#                                        dyna-net*.c -> net (the net family)
#                                        dyna-uring.c -> uring
#   src/core|builtins|vm|parser|serialize|runtime|value|object|mm|compat|fuzz/*,
#   src/dynajs.c src/engine-*.c src/dyna-js* src/cutils.* src/dtoa.*
#   src/libregexp* src/libunicode*, build.sh
#                             -> core (engine-wide: the full matrix)
#   tests/blackbox/bb_<m>.js  -> <m>
#   tests/test_<m>*.js        -> <m>  (the name up to the first underscore:
#                                        test_net_tcp.js -> net)
#   install.sh tools/*        -> infra (dev.sh, the compat shim, too)
#   anything else             -> core (conservative: engine-wide)
set -u

BASE=${1:-HEAD}

files=$( { git diff --name-only "$BASE" 2>/dev/null || {
            echo "affected-modules: cannot diff '$BASE' -- falling back to the working tree (HEAD)" >&2
            git diff --name-only HEAD 2>/dev/null; }; \
          git ls-files --others --exclude-standard -- src tests 2>/dev/null; } | sort -u )

mods=""
add(){
  case " $mods " in
    *" $1 "*) ;;
    *) mods="$mods $1" ;;
  esac
}

for f in $files; do
  case "$f" in
    # -- module sources; the special cases must come before the generic rule
    src/dyna-simd*)   add simd ;;
    src/dyna-libc.*)  add std; add os ;;
    src/dyna-net*.c)  add net ;;
    src/dyna-uring.c) add uring ;;
    src/dyna-*.inc.c|src/dyna-*.c|src/dyna-*.h)
        b=${f#src/dyna-}; b=${b%%.*}; add "$b" ;;
    # -- engine core (and the build that wires it): the full matrix owns these
    src/core/*|src/builtins/*|src/vm/*|src/parser/*|src/serialize/*|\
    src/runtime/*|src/value/*|src/object/*|src/mm/*|src/compat/*|src/fuzz/*|\
    src/dynajs.c|src/dynajs.h|src/engine-*.c|src/dyna-js*|\
    src/cutils.*|src/dtoa.*|src/libregexp*|src/libunicode*) add core ;;
    # -- the build system itself: engine-wide (build.sh holds the whole build)
    build.sh) add core ;;
    # -- the suites themselves; a suite change scopes to (at least) itself
    tests/blackbox/bb_*.js) b=${f##*/bb_}; b=${b%.js}; add "$b" ;;
    tests/test_*.js) b=${f##*/}; b=${b#test_}; b=${b%%_*}; add "$b" ;;
    # -- gate/build machinery: not a module; the callers decide
    dev.sh|install.sh|tools/*) add infra ;;
    # -- conservative fallback: engine-wide
    *) add core ;;
  esac
done

core=""
rest=""
for m in $mods; do
  if [ "$m" = core ]; then core=core
  else rest="$rest $m"
  fi
done
sorted=$(printf '%s\n' $rest | sort | tr '\n' ' ')
out="$sorted"
[ -n "$core" ] && out="core $out"
printf '%s\n' "$out" | sed 's/ *$//'
