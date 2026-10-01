#!/bin/sh
# dynajs-prepush-hook -- installed by `./build.sh install-hooks`. Do not edit here;
# the tracked source is tools/pre-push-hook.sh (edit that, re-run install-hooks).
#
# A hook that exits 0 when it cannot run is not a hook, so this one fails
# closed. To push past a known-red gate, and only then:  git push --no-verify
#
# The full gate costs ~15 minutes; a push whose commits change no executable
# input pays for nothing. git feeds this hook one line per pushed ref on
# stdin:  "<local-ref> <local-sha> <remote-ref> <remote-sha>". For each
# range that touches a code path, run `./build.sh gate`; a range that touched
# only docs/config (README.md, CLAUDE.md, Changelog, .gitignore, audit/)
# skips with a line naming the decision. Deleting a remote ref changes no
# code and never gates. A new branch has nothing to diff against: gate it.
# (the gate itself IS `./build.sh gate` -- the 12-stage wave proof absorbed
# into build.sh, plus the test262 corpus leg; `prepush` is an alias)
#
# FORCE_PREPUSH=1 forces the gate even for a docs/config-only push (the skip
# line names it). When the gate runs it execs `./build.sh gate`:
# per-stage logs land in .prepush/<stage>.log and one failed stage re-runs
# alone with:  ./build.sh __gate-stage <name>
set -eu
cd "$(git rev-parse --show-toplevel)"

CODE_PATHS='src tests'

run_gate=0
while read -r local_ref local_sha remote_ref remote_sha; do
    [ -n "${local_sha:-}" ] || { run_gate=1; continue; }
    case "$local_sha" in
    0000000000000000000000000000000000000000)
        continue ;;
    esac
    case "$remote_sha" in
    0000000000000000000000000000000000000000|'')
        run_gate=1; continue ;;
    esac
    if git diff --quiet "$remote_sha" "$local_sha" -- $CODE_PATHS; then
        echo "pre-push: $local_ref -> $remote_ref touches no code path -- gate skipped for this range"
    else
        run_gate=1
    fi
done

if [ "${FORCE_PREPUSH:-0}" = 1 ]; then
    run_gate=1
fi
if [ "$run_gate" -eq 0 ]; then
    echo "pre-push: docs/config-only push -- gate skipped (use FORCE_PREPUSH=1 to run it anyway)"
    exit 0
fi
echo "pre-push: running ./build.sh gate (use --no-verify to skip)"
echo "pre-push: per-stage logs land in .prepush/<stage>.log; re-run one failed stage with:"
echo "pre-push:   ./build.sh __gate-stage <name>"
exec ./build.sh gate
