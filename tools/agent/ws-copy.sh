#!/bin/sh
# ws-copy.sh -- include-list workspace copies for parallel fix agents.
#
# Implements AGENT.md section 2 for this repo: every agent works in its own
# full copy of the workspace placed INSIDE the project (gitignored
# .agent-work/ws/<name>), never in /tmp. The copy is made from an INCLUDE
# list, never exclude patterns: the list is materialized at copy time as
# every git-tracked or non-ignored file (git ls-files --cached --others
# --exclude-standard, so project content added later is picked up
# automatically) plus these named extras that git ignores but the build,
# gates or protocol require:
#     bench/_sec.js            the security-test table source (gitignored bench/*)
#     AGENT.md                 the sub-agent protocol document
#     .agent-work/audit-plan   lane findings + probe scripts (agent reference, read-only)
# The materialized list is kept beside the workspace as <name>.include.txt
# so every copy is auditable file by file.
#
# Usage:
#   tools/agent/ws-copy.sh create NAME        copy + git init + base commit
#   tools/agent/ws-copy.sh link262 NAME       symlink the main tree's test262 fixtures (read-only)
#   tools/agent/ws-copy.sh patch NAME         write <ws>/<name>.patch (git diff base --binary)
#   tools/agent/ws-copy.sh merge NAME         apply <name>.patch to the main tree (orchestrator only)
#   tools/agent/ws-copy.sh list               workspaces + dirty/clean + age
#   tools/agent/ws-copy.sh drop NAME          delete one workspace
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WS_HOME=${DYNA_WS_HOME:-$ROOT/.agent-work/ws}
EXTRAS="bench/_sec.js AGENT.md .agent-work/audit-plan"

die() { echo "ws-copy: $*" >&2; exit 1; }
ws_dir() { echo "$WS_HOME/$1"; }

materialize_include_list() {
    _list=$1
    {
        git -C "$ROOT" ls-files --cached --others --exclude-standard
        for x in $EXTRAS; do
            if [ -e "$ROOT/$x" ]; then find "$ROOT/$x" -type f; fi
        done
    } | sed "s|^$ROOT/||" | sort -u
}

cmd=${1:-}; name=${2:-}
case "$cmd" in
create)
    [ -n "$name" ] || die "usage: ws-copy.sh create NAME"
    [ -d "$WS_HOME/$name" ] && die "workspace already exists: $name"
    [ -f "$ROOT/tools/agent/ws-copy.sh" ] || die "not run from the repo layout"
    mkdir -p "$WS_HOME"
    LIST="$WS_HOME/$name.include.txt"
    materialize_include_list "$LIST" > "$LIST"
    [ "$(wc -l < "$LIST")" -gt 100 ] || die "include list suspiciously small: $LIST"
    mkdir -p "$(ws_dir "$name")"
    rsync -a --files-from="$LIST" "$ROOT/" "$(ws_dir "$name")/"
    touch "$(ws_dir "$name")/.ws-marker"
    echo ".ws-marker" >> "$(ws_dir "$name")/.gitignore" 2>/dev/null || true
    if git -C "$(ws_dir "$name")" init -q 2>/dev/null; then
        git -C "$(ws_dir "$name")" add -A
        git -C "$(ws_dir "$name")" -c user.email=ws@local -c user.name=ws \
            commit -qm "workspace base (include-list copy of $ROOT)"
    else
        touch "$(ws_dir "$name")/.ws-nogit"
    fi
    date '+%Y-%m-%d %H:%M:%S created workspace '"$name" > "$(ws_dir "$name")/progress.log"
    printf '# %s -- handover changelog (every approach TRIED, MISSED, POSTPONED, every gate with numbers)\n' "$name" \
        > "$(ws_dir "$name")/CHANGELOG.md"
    echo "$(ws_dir "$name")"
    ;;
link262)
    [ -n "$name" ] || die "usage: ws-copy.sh link262 NAME"
    [ -d "$ROOT/test262" ] || die "no test262 fixtures in the main tree"
    [ -d "$(ws_dir "$name")" ] || die "no workspace: $name"
    [ -e "$(ws_dir "$name")/test262" ] || ln -s "$ROOT/test262" "$(ws_dir "$name")/test262"
    echo "$(ws_dir "$name")/test262 -> $ROOT/test262 (read-only fixtures)"
    ;;
patch)
    [ -n "$name" ] || die "usage: ws-copy.sh patch NAME"
    [ -d "$(ws_dir "$name")/.git" ] || die "workspace has no git: $name"
    base=$(git -C "$(ws_dir "$name")" rev-list --max-parents=0 HEAD | tail -1)
    git -C "$(ws_dir "$name")" diff "$base" HEAD --binary > "$(ws_dir "$name")/$name.patch"
    echo "$(ws_dir "$name")/$name.patch ($(wc -c < "$(ws_dir "$name")/$name.patch") bytes)"
    ;;
merge)
    [ -n "$name" ] || die "usage: ws-copy.sh merge NAME"
    [ -f "$(ws_dir "$name")/$name.patch" ] || die "no patch yet: run patch first"
    git -C "$ROOT" apply --index "$(ws_dir "$name")/$name.patch" 2>/dev/null \
        || git -C "$ROOT" apply "$(ws_dir "$name")/$name.patch" \
        || die "patch did not apply cleanly to the main tree"
    echo "merged $(ws_dir "$name")/$name.patch into $ROOT (not committed)"
    ;;
list)
    [ -d "$WS_HOME" ] || { echo "(no workspaces in $WS_HOME)"; exit 0; }
    for d in "$WS_HOME"/*/; do
        [ -d "$d" ] || continue
        n=$(basename "$d")
        st=clean
        b=$(git -C "$d" rev-list --max-parents=0 HEAD 2>/dev/null | tail -1)
        [ -n "$b" ] && { git -C "$d" diff --quiet "$b" HEAD 2>/dev/null || st=DIRTY; }
        printf '%-16s %-8s %s\n' "$n" "$st" "$d"
    done
    ;;
drop)
    [ -n "$name" ] || die "usage: ws-copy.sh drop NAME"
    rm -rf "$(ws_dir "$name")" "$WS_HOME/$name.include.txt"
    echo "dropped $name"
    ;;
*)
    die "unknown command: $cmd (create|link262|patch|merge|list|drop)"
    ;;
esac
