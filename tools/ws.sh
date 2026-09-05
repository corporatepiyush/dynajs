#!/bin/sh
# ws.sh -- isolated concurrent workspaces for parallel agents.
#
# The coordinator/agent split this repo has used gives agents disjoint FILE
# ownership on the SHARED tree and serializes all builds on the coordinator.
# That is safe but wastes the machine: N agents, one builder. This tool is
# the other architecture: each agent gets a full WORKSPACE COPY in its own
# temp directory and may build, test and mutate it freely; the main tree is
# read-only for agents; convergence is PATCH-BASED, applied by the
# coordinator, serially, after review -- so conflicts surface at merge time
# as patch failures, never as clobbered objects mid-build.
#
# Why plain copies and not git worktrees: worktrees share one .git, so
# parallel agents contend on index.lock, branch checkouts are mutually
# exclusive, and a careless `git clean` in one reaches files another is
# editing. A workspace here gets a FRESH `git init` seeded with the base
# snapshot: `ws diff` is then a complete, binary-safe patch of exactly what
# the agent changed, and merging never touches shared git state.
#
# The copy excludes what a workspace rebuilds or cannot use (.obj, the built
# binaries, test262 -- 265 MB of test fixtures the engine tests fetch from
# the main tree, core dumps) and keeps everything else, so `make`, `ccheck`
# and the per-module targets work unmodified inside the workspace: OBJDIR is
# relative and every path in the Makefile resolves from the workspace root.
#
# Usage (from any tree that contains this script's repo layout):
#   tools/ws.sh create  mywork [SRC]   # SRC defaults to the tree above tools/
#   tools/ws.sh list                    # workspaces + dirty/clean + age
#   tools/ws.sh shell   mywork          # print the workspace path
#   tools/ws.sh diff    mywork [FILE]   # patch vs base (stdout or FILE)
#   tools/ws.sh merge   mywork          # apply patch to the SOURCE tree
#   tools/ws.sh reset   mywork          # discard changes, back to base
#   tools/ws.sh drop    mywork          # delete the workspace
#   tools/ws.sh drop    --all           # delete every workspace (are-you-sure)
#
# WS_HOME (default /tmp/dynaws-<basename>) places the workspaces; set
# DYNA_WS_HOME to override. Everything is safe to run concurrently across
# agents EXCEPT merge/reset/drop on the SAME name.

set -u

# The SOURCE tree this script belongs to: derived from the script's own
# location, NOT the caller's CWD, so a workspace copy (which carries its own
# tools/ws.sh) merges back to ITSELF during self-tests rather than to the
# original -- and an agent who runs the wrong ws.sh still cannot escape its
# workspace.
SELF=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$SELF/.." && pwd)
WS_HOME=${DYNA_WS_HOME:-/tmp/dynaws-$(basename "$SRC")}
# A workspace must never recursively contain workspaces of its own.
# LEADING SLASHES anchor patterns to the transfer root: a bare `core`
# would also eat src/core/ -- the exact silent-narrowing mistake that first
# broke the workspace self-build.
EXCLUDES="--exclude /.obj --exclude /test262 --exclude /.git --exclude /.dev
  --exclude /.swarm --exclude /.commandcode --exclude /.zcode --exclude /core
  --exclude '/core.*' --exclude '*.dSYM' --exclude /.build-variant"

die() { echo "ws: $*" >&2; exit 1; }

ws_git() { git -C "$WS_HOME/$1" "${@:2}"; }

need_ws() {
    [ -d "$WS_HOME/$1" ] || die "no workspace '$1' in $WS_HOME (see: ws list)"
}

cmd_create() {
    [ $# -ge 1 ] || die "create NAME [SRC]"
    local name=$1 src=${2:-$SRC}
    [ -d "$WS_HOME/$name" ] && die "workspace '$name' already exists"
    # Refuse to nest: creating from a workspace tree would let an agent
    # fork workspaces from its own divergent copy.
    [ -f "$src/tools/ws.sh" ] || die "$src has no tools/ws.sh -- not a ws tree"
    [ -d "$src/.ws-marker" ] && die "refusing to nest workspaces"
    mkdir -p "$WS_HOME" || die "cannot mkdir $WS_HOME"
    # rsync (not cp -c): exclusions are the whole point -- a volume mount
    # ignores .dockerignore and CoW copies cannot exclude, so a naive clone
    # drags 300+ MB per agent. This is the measured lesson from the gate's
    # own tree clones, applied per-workspace.
    rsync -a $EXCLUDES "$src/" "$WS_HOME/$name/" || die "copy failed"
    # Fresh git: the base snapshot is the merge ANCESTOR. Binary-safe diffs,
    # no shared state with the source's .git, no branch contention.
    if git -C "$WS_HOME/$name" init -q 2>/dev/null; then
        : > "$WS_HOME/$name/.ws-marker"
        echo ".ws-marker" >> "$WS_HOME/$name/.gitignore" 2>/dev/null || true
        git -C "$WS_HOME/$name" add -A
        git -C "$WS_HOME/$name" -c user.email=ws@local -c user.name=ws \
            commit -qm "workspace base (from $src)" || true
    else
        # No git in PATH: the workspace still works, diff/merge refuse
        # loudly rather than guessing (a merge without an ancestor is a
        # guess wearing a tool's name).
        touch "$WS_HOME/$name/.ws-nogit"
    fi
    echo "$WS_HOME/$name"
}

cmd_list() {
    [ -d "$WS_HOME" ] || { echo "(no workspaces in $WS_HOME)"; exit 0; }
    printf '%-16s %-8s %-10s %s\n' NAME STATE AGE PATH
    for d in "$WS_HOME"/*/; do
        local name=$(basename "$d")
        [ -f "$d/.ws-marker" ] || continue
        local state=clean
        [ -f "$d/.ws-nogit" ] && state=nogit
        [ "$state" = clean ] && [ -n "$(git -C "$d" status --porcelain 2>/dev/null)" ] && state=DIRTY
        printf '%-16s %-8s %-10s %s\n' "$name" "$state" \
            "$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$d" 2>/dev/null || stat -c '%y' "$d" 2>/dev/null | cut -d. -f1)" "$d"
    done
}

cmd_diff() {
    need_ws "$1"
    local out=${2:-}
    [ -f "$WS_HOME/$1/.ws-nogit" ] && die "workspace has no git ancestor (nogit)"
    if [ -n "$out" ]; then
        ws_git "$1" diff --binary > "$out" || die "diff failed"
        echo "wrote $out ($(wc -l < "$out") lines)"
    else
        ws_git "$1" diff --binary
    fi
}

cmd_merge() {
    need_ws "$1"
    [ -f "$WS_HOME/$1/.ws-nogit" ] && die "workspace has no git ancestor (nogit)"
    # The patch is generated against the workspace BASE, i.e. the state of
    # SRC at create time. Applying to a SRC that has since moved is exactly
    # a three-way situation git-apply resolves per-hunk: --3way needs the
    # blobs, so fall back to a strict forward apply and REFUSE on any
    # conflict -- a silent partial merge is worse than a loud none.
    ws_git "$1" diff --binary > "$WS_HOME/$1/.merge.patch" || die "diff failed"
    [ -s "$WS_HOME/$1/.merge.patch" ] || { echo "ws: nothing to merge"; exit 0; }
    if git -C "$SRC" apply --check --binary "$WS_HOME/$1/.merge.patch" 2>/dev/null; then
        git -C "$SRC" apply --binary "$WS_HOME/$1/.merge.patch" || die "apply failed after check passed"
        echo "merged $1 -> $SRC (review + build + test before committing)"
    else
        die "patch does not apply cleanly to $SRC (diverged?) -- resolve by hand:
  patch file: $WS_HOME/$1/.merge.patch
  inspect:    git -C '$SRC' apply --stat --binary '$WS_HOME/$1/.merge.patch'"
    fi
}

cmd_reset() {
    need_ws "$1"
    [ -f "$WS_HOME/$1/.ws-nogit" ] && die "nogit workspace: drop and recreate"
    ws_git "$1" add -A
    ws_git "$1" -c user.email=ws@local -c user.name=ws commit -qm "wip (auto before reset)" || true
    # Reset to the BASE commit specifically, not HEAD~: resets must return
    # to the create-time ancestor even after the agent committed.
    ws_git "$1" reset --hard -q "$(ws_git "$1" rev-list --max-parents=0 HEAD)" || die "reset failed"
    echo "reset $1 to base"
}

cmd_drop() {
    [ $# -ge 1 ] || die "drop NAME|--all"
    if [ "$1" = "--all" ]; then
        [ -d "$WS_HOME" ] || die "nothing to drop"
        rm -rf "$WS_HOME"
        echo "dropped all workspaces under $WS_HOME"
        return 0
    fi
    need_ws "$1"
    rm -rf "$WS_HOME/$1"
    echo "dropped $1"
}

cmd_shell() { need_ws "$1"; echo "$WS_HOME/$1"; }

case "${1:-}" in
    create) shift; cmd_create "$@" ;;
    list|ls) cmd_list ;;
    diff) shift; cmd_diff "$@" ;;
    merge) shift; cmd_merge "$@" ;;
    reset) shift; cmd_reset "$@" ;;
    drop|rm) shift; cmd_drop "$@" ;;
    shell) shift; cmd_shell "$@" ;;
    *) sed -n '2,30p' "$0"; exit 1 ;;
esac
