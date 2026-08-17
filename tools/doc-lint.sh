#!/bin/sh
# doc-lint.sh -- user-facing docs state what the API IS, in the present tense.
#
# No "deferred", no "not implemented", no "used to live", no "moved here from",
# no migration notes. A reference documents the surface as it exists; gaps
# belong in a plan, where they can be scheduled, not in the page a user reads to
# find out what a function does.
#
# Exit 0 = clean. Any hit prints file:line and exits 1.
set -eu
DOCS=${1:-docs}
# Plans are where gaps belong (see above); a plan saying "not implemented" is
# the lint's own escape hatch, not a violation. Everything else is a reference.
BANNED='not implemented|Not implemented|\(deferred\)|used to live|moved here from|moved to \[|is deprecated|for backward compat|backwards compat'
rc=0
if hits=$(grep -rnE "$BANNED" "$DOCS" --include='*.md' 2>/dev/null \
        | grep -v '^docs/report/'); then
    echo "doc-lint: FAIL -- user-facing docs must state what the API is:"
    echo "$hits" | sed 's/^/    /'
    rc=1
fi
[ "$rc" -eq 0 ] && echo "doc-lint: OK ($DOCS is in the present tense)"
exit "$rc"
