#!/bin/sh
# dynajs-prepush-hook -- installed by `make install-hooks`. Do not edit here;
# the tracked source is tools/pre-push-hook.sh (edit that, re-run install-hooks).
#
# A hook that exits 0 when it cannot run is not a hook, so this one fails
# closed. To push past a known-red gate, and only then:  git push --no-verify
#
# The full gate costs ~5 minutes; a push whose commits change no executable
# input pays for nothing. git feeds this hook one line per pushed ref on
# stdin:  "<local-ref> <local-sha> <remote-ref> <remote-sha>". For each
# range that touches a code path, run `make prepush`; a range that touched
# only docs/config (README.md, CLAUDE.md, Changelog, .gitignore, audit/)
# skips with a line naming the decision. Deleting a remote ref changes no
# code and never gates. A new branch has nothing to diff against: gate it.
set -eu
cd "$(git rev-parse --show-toplevel)"

# What counts as "code": SOURCE FILES and TEST FILES, exactly. Anything
# else (Makefile, docs, tools, docker, config) pushes without the gate.
CODE_PATHS='src tests'

run_gate=0
while read -r local_ref local_sha remote_ref remote_sha; do
    [ -n "${local_sha:-}" ] || { run_gate=1; continue; }   # malformed line: gate
    case "$local_sha" in
    0000000000000000000000000000000000000000)
        continue ;;                                          # ref deletion: skip
    esac
    case "$remote_sha" in
    0000000000000000000000000000000000000000|'')
        run_gate=1; continue ;;                              # new branch: gate
    esac
    if git diff --quiet "$remote_sha" "$local_sha" -- $CODE_PATHS; then
        echo "pre-push: $local_ref -> $remote_ref touches no code path -- gate skipped for this range"
    else
        run_gate=1
    fi
done

if [ "$run_gate" -eq 0 ]; then
    echo "pre-push: docs/config-only push -- gate skipped (use FORCE_PREPUSH=1 to override)"
    exit 0
fi
echo "pre-push: running make prepush (use --no-verify to skip)"
exec make prepush
